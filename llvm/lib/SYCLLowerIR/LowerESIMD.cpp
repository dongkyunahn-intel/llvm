//===-- LowerESIMD.cpp - lower Explicit SIMD (ESIMD) constructs -----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// See intro comments in the header.
//
// Since the spir* targets use Itanium mangling for C/C++ symbols, the
// implementation uses the Itanium demangler to demangle device code's
// C++ intrinsics and access various information, such their C++ names and
// values of integer template parameters they were instantiated with.
//===----------------------------------------------------------------------===//

#include "llvm/SYCLLowerIR/LowerESIMD.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Demangle/ItaniumDemangle.h"
#include "llvm/GenXIntrinsics/GenXIntrinsics.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"

#include <cctype>
#include <cstring>
#include <unordered_map>

using namespace llvm;
namespace id = itanium_demangle;

#define DEBUG_TYPE "lower-esimd"

#define SLM_BTI 254

namespace {
class SYCLLowerESIMDLegacyPass : public FunctionPass {
public:
  static char ID; // Pass identification, replacement for typeid
  SYCLLowerESIMDLegacyPass() : FunctionPass(ID) {
    initializeSYCLLowerESIMDLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  // run the LowerESIMD pass on the specified module
  bool runOnFunction(Function &F) override {
    FunctionAnalysisManager FAM;
    auto PA = Impl.run(F, FAM, GenXVolatileTypeSet);
    return !PA.areAllPreserved();
  }

  bool doInitialization(Module &M) override {
    // emit ESIMD backend compatible metadata.
    generateKernelMetadata(M);
    collectGenXVolatileType(M);
    return false;
  }

private:
  SYCLLowerESIMDPass Impl;
  SmallPtrSet<Type *, 4> GenXVolatileTypeSet;
  void generateKernelMetadata(Module &M);
  void collectGenXVolatileType(Module &M);
};
} // namespace

char SYCLLowerESIMDLegacyPass::ID = 0;
INITIALIZE_PASS(SYCLLowerESIMDLegacyPass, "LowerESIMD",
                "Lower constructs specific to Close To Metal", false, false)

// Public interface to the SYCLLowerESIMDPass.
FunctionPass *llvm::createSYCLLowerESIMDPass() {
  return new SYCLLowerESIMDLegacyPass();
}

namespace {
// The regexp for ESIMD intrinsics:
// /^_Z(\d+)__esimd_\w+/
static constexpr char ESIMD_INTRIN_PREF0[] = "_Z";
static constexpr char ESIMD_INTRIN_PREF1[] = "__esimd_";
static constexpr char SPIRV_INTRIN_PREF[] = "__spirv_";

static constexpr char GENX_KERNEL_METADATA[] = "genx.kernels";

struct ESIMDIntrinDesc {
  // Denotes argument translation rule kind.
  enum GenXArgRuleKind {
    SRC_CALL_ARG, // is a call argument
    SRC_CALL_ALL, // this and subsequent args are just copied from the src call
    SRC_TMPL_ARG, // is an integer template argument
    NUM_BYTES,    // is a number of bytes (gather.scaled and scatter.scaled)
    UNDEF,        // is an undef value
    CONST_INT16,  // is an i16 constant
    CONST_INT32,  // is an i32 constant
    CONST_INT64,  // is an i64 constant
  };

  enum GenXArgConversion {
    NONE,  // no conversion
    TO_I1, // convert vector of N-bit integer to 1-bit
    TO_SI  // convert to 32-bit integer surface index
  };

  // Denotes GenX intrinsic name suffix creation rule kind.
  enum GenXSuffixRuleKind {
    NO_RULE,
    BIN_OP,  // ".<binary operation>" - e.g. "*.add"
    NUM_KIND // "<numeric kind>" - e.g. "*i" for integer, "*f" for float
  };

  // Represents a rule how a GenX intrinsic argument is created from the source
  // call instruction.
  struct ArgRule {
    GenXArgRuleKind Kind;
    union Info {
      struct {
        int16_t CallArgNo; // SRC_CALL_ARG: source call arg num
                           // UNDEF: source call arg num to get type from
                           // -1 denotes return value
        int16_t Conv;      // GenXArgConversion
      } Arg;
      int NRemArgs;           // SRC_CALL_ALL: number of remaining args
      unsigned int TmplArgNo; // SRC_TMPL_ARG: source template arg num
      unsigned int ArgConst;  // CONST_I16 OR CONST_I32: constant value
    } I;
  };

  // Represents a rule how a GenX intrinsic name suffix is created from the
  // source call instruction.
  struct NameRule {
    GenXSuffixRuleKind Kind;
    union Info {
      int CallArgNo; // DATA_TYPE: source call arg num to get type from
      int TmplArgNo; // BINOP: source template arg num denoting the binary op
    } I;
  };

  std::string GenXSpelling;
  SmallVector<ArgRule, 16> ArgRules;
  NameRule SuffixRule = {NO_RULE, 0};

  int getNumGenXArgs() const {
    auto NRules = ArgRules.size();

    if (NRules == 0)
      return 0;

    // SRC_CALL_ALL is a "shortcut" to save typing, must be the last rule
    if (ArgRules[NRules - 1].Kind == GenXArgRuleKind::SRC_CALL_ALL)
      return ArgRules[NRules - 1].I.NRemArgs + (NRules - 1);
    return NRules;
  }

  bool isValid() const { return !GenXSpelling.empty(); }
};

using IntrinTable = std::unordered_map<std::string, ESIMDIntrinDesc>;

class ESIMDIntrinDescTable {
private:
  IntrinTable Table;

#define DEF_ARG_RULE(Nm, Kind)                                                 \
  static constexpr ESIMDIntrinDesc::ArgRule Nm(int16_t N) {                    \
    return ESIMDIntrinDesc::ArgRule{ESIMDIntrinDesc::Kind, N};                 \
  }
  DEF_ARG_RULE(l, SRC_CALL_ALL)
  DEF_ARG_RULE(t, SRC_TMPL_ARG)
  DEF_ARG_RULE(u, UNDEF)
  DEF_ARG_RULE(nbs, NUM_BYTES)

  static constexpr ESIMDIntrinDesc::ArgRule a(int16_t N) {
    return ESIMDIntrinDesc::ArgRule{
        ESIMDIntrinDesc::SRC_CALL_ARG,
        {N, ESIMDIntrinDesc::GenXArgConversion::NONE}};
  }

  static constexpr ESIMDIntrinDesc::ArgRule ai1(int16_t N) {
    return ESIMDIntrinDesc::ArgRule{
        ESIMDIntrinDesc::SRC_CALL_ARG,
        {N, ESIMDIntrinDesc::GenXArgConversion::TO_I1}};
  }

  static constexpr ESIMDIntrinDesc::ArgRule aSI(int16_t N) {
    return ESIMDIntrinDesc::ArgRule{
        ESIMDIntrinDesc::SRC_CALL_ARG,
        {N, ESIMDIntrinDesc::GenXArgConversion::TO_SI}};
  }

  static constexpr ESIMDIntrinDesc::ArgRule c16(int16_t N) {
    return ESIMDIntrinDesc::ArgRule{ESIMDIntrinDesc::CONST_INT16, N};
  }

  static constexpr ESIMDIntrinDesc::ArgRule c32(int16_t N) {
    return ESIMDIntrinDesc::ArgRule{ESIMDIntrinDesc::CONST_INT32, N};
  }

  static constexpr ESIMDIntrinDesc::ArgRule c64(int16_t N) {
    return ESIMDIntrinDesc::ArgRule{ESIMDIntrinDesc::CONST_INT64, N};
  }

  static constexpr ESIMDIntrinDesc::NameRule bo(int16_t N) {
    return ESIMDIntrinDesc::NameRule{ESIMDIntrinDesc::BIN_OP, N};
  }

  static constexpr ESIMDIntrinDesc::NameRule nk(int16_t N) {
    return ESIMDIntrinDesc::NameRule{ESIMDIntrinDesc::NUM_KIND, N};
  }

public:
  ESIMDIntrinDescTable() {
    Table = {
        // An element of the table is std::pair of <key, value>; key is the
        // source
        // spelling of and intrinsic (what follows the "__esimd_" prefix), and
        // the
        // value is an instance of the ESIMDIntrinDesc class.
        // Example for the "rdregion" intrinsic encoding:
        // "rdregion" - the GenX spelling of the intrinsic ("llvm.genx." prefix
        //      and type suffixes maybe added to get full GenX name)
        // {a(0), t(3),...}
        //      defines a map from the resulting genx.* intrinsic call arguments
        //      to the source call's template or function call arguments, e.g.
        //      0th genx arg - maps to 0th source call arg
        //      1st genx arg - maps to 3rd template argument of the source call
        // nk(N) or bo(N)
        //      a rule applied to the base intrinsic name in order to
        //      construct a full name ("llvm.genx." prefix s also added); e.g.
        //      - nk(-1) denotes adding the return type name-based suffix - "i"
        //          for integer, "f" - for floating point
        {"rdregion",
         {"rdregion", {a(0), t(3), t(4), t(5), a(1), t(6)}, nk(-1)}},
        {{"wrregion"},
         {{"wrregion"},
          {a(0), a(1), t(3), t(4), t(5), a(2), t(6), ai1(3)},
          nk(-1)}},
        {"vload", {"vload", {l(0)}}},
        {"vstore", {"vstore", {a(1), a(0)}}},

        {"flat_block_read_unaligned", {"svm.block.ld.unaligned", {l(0)}}},
        {"flat_block_write", {"svm.block.st", {l(1)}}},
        {"flat_read", {"svm.gather", {ai1(2), a(1), a(0), u(-1)}}},
        {"flat_read4",
         {"svm.gather4.scaled", {ai1(1), t(2), c16(0), c64(0), a(0), u(-1)}}},
        {"flat_write", {"svm.scatter", {ai1(3), a(2), a(0), a(1)}}},
        {"flat_write4",
         {"svm.scatter4.scaled", {ai1(2), t(2), c16(0), c64(0), a(0), a(1)}}},

        // surface index-based gather/scatter:
        // num blocks, scale, surface index, global offset, elem offsets
        {"surf_read", {"gather.scaled2", {t(3), c16(0), aSI(1), a(2), a(3)}}},
        // pred, num blocks, scale, surface index, global offset, elem offsets,
        // data to write
        {"surf_write",
         {"scatter.scaled", {ai1(0), t(3), c16(0), aSI(2), a(3), a(4), a(5)}}},

        // intrinsics to query thread's coordinates:
        {"group_id_x", {"group.id.x", {}}},
        {"group_id_y", {"group.id.y", {}}},
        {"group_id_z", {"group.id.z", {}}},
        {"local_id", {"local.id", {}}},
        {"local_size", {"local.size", {}}},
        {"flat_atomic0", {"svm.atomic", {ai1(1), a(0), u(-1)}, bo(0)}},
        {"flat_atomic1", {"svm.atomic", {ai1(2), a(0), a(1), u(-1)}, bo(0)}},
        {"flat_atomic2",
         {"svm.atomic", {ai1(3), a(0), a(1), a(2), u(-1)}, bo(0)}},
        {"reduced_fmax", {"fmax", {a(0), a(1)}}},
        {"reduced_umax", {"umax", {a(0), a(1)}}},
        {"reduced_smax", {"smax", {a(0), a(1)}}},
        {"reduced_fmin", {"fmin", {a(0), a(1)}}},
        {"reduced_umin", {"umin", {a(0), a(1)}}},
        {"reduced_smin", {"smin", {a(0), a(1)}}},
        {"dp4", {"dp4", {a(0), a(1)}}},
        // 2nd argumnent of media.* is a surface index -
        // it is produced by casting and truncating the OpenCL opaque image
        // pointer
        // source media_block* intrinsic argument; this is according the the
        // OpenCL runtime - JIT compiler handshake protocol for OpenCL images.
        {"media_block_load",
         {"media.ld", {a(0), aSI(1), a(2), a(3), a(4), a(5)}}},
        {"media_block_store",
         {"media.st", {a(0), aSI(1), a(2), a(3), a(4), a(5), a(6)}}},
        {"slm_fence", {"fence", {a(0)}}},
        {"barrier", {"barrier", {}}},
        {"block_read", {"oword.ld.unaligned", {c32(0), aSI(0), a(1)}}},
        {"block_write", {"oword.st", {aSI(0), a(1), a(2)}}},
        {"slm_block_read",
         {"oword.ld.unaligned", {c32(0), c32(SLM_BTI), a(0)}}},
        {"slm_block_write", {"oword.st", {c32(SLM_BTI), a(0), a(1)}}},
        {"slm_read",
         {"gather.scaled",
          {ai1(1), nbs(-1), c16(0), c32(SLM_BTI), c32(0), a(0), u(-1)}}},
        {"slm_read4",
         {"gather4.scaled",
          {ai1(1), t(2), c16(0), c32(SLM_BTI), c32(0), a(0), u(-1)}}},
        {"slm_write",
         {"scatter.scaled",
          {ai1(2), nbs(1), c16(0), c32(SLM_BTI), c32(0), a(0), a(1)}}},
        {"slm_write4",
         {"scatter4.scaled",
          {ai1(2), t(2), c16(0), c32(SLM_BTI), c32(0), a(0), a(1)}}},
        {"slm_atomic0",
         {"dword.atomic", {ai1(1), c32(SLM_BTI), a(0), u(-1)}, bo(0)}},
        {"slm_atomic1",
         {"dword.atomic", {ai1(2), c32(SLM_BTI), a(0), a(1), u(-1)}, bo(0)}},
        {"slm_atomic2",
         {"dword.atomic",
          {ai1(3), c32(SLM_BTI), a(0), a(1), a(2), u(-1)},
          bo(0)}},
        {"raw_sends_load",
         {"raw.sends2",
          {a(0), a(1), ai1(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9), a(10),
           a(11)}}},
        {"raw_send_load",
         {"raw.send2",
          {a(0), a(1), ai1(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9)}}},
        {"raw_sends_store",
         {"raw.sends2.noresult",
          {a(0), a(1), ai1(2), a(3), a(4), a(5), a(6), a(7), a(8), a(9)}}},
        {"raw_send_store",
         {"raw.send2.noresult",
          {a(0), a(1), ai1(2), a(3), a(4), a(5), a(6), a(7)}}},
        {"satf", {"sat", {a(0)}}},
        {"fptoui_sat", {"fptoui.sat", {a(0)}}},
        {"fptosi_sat", {"fptosi.sat", {a(0)}}},
        {"uutrunc_sat", {"uutrunc.sat", {a(0)}}},
        {"ustrunc_sat", {"ustrunc.sat", {a(0)}}},
        {"sutrunc_sat", {"sutrunc.sat", {a(0)}}},
        {"sstrunc_sat", {"sstrunc.sat", {a(0)}}},
        {"abs", {"abs", {a(0)}, nk(-1)}},
        {"ssshl", {"ssshl", {a(0), a(1)}}},
        {"sushl", {"sushl", {a(0), a(1)}}},
        {"usshl", {"usshl", {a(0), a(1)}}},
        {"uushl", {"uushl", {a(0), a(1)}}},
        {"ssshl_sat", {"ssshl.sat", {a(0), a(1)}}},
        {"sushl_sat", {"sushl.sat", {a(0), a(1)}}},
        {"usshl_sat", {"usshl.sat", {a(0), a(1)}}},
        {"uushl_sat", {"uushl.sat", {a(0), a(1)}}},
        {"rol", {"rol", {a(0), a(1)}}},
        {"ror", {"ror", {a(0), a(1)}}},
        {"umulh", {"umulh", {a(0), a(1)}}},
        {"smulh", {"smulh", {a(0), a(1)}}},
        {"frc", {"frc", {a(0)}}},
        {"fmax", {"fmax", {a(0), a(1)}}},
        {"umax", {"umax", {a(0), a(1)}}},
        {"smax", {"smax", {a(0), a(1)}}},
        {"lzd", {"lzd", {a(0)}}},
        {"fmin", {"fmin", {a(0), a(1)}}},
        {"umin", {"umin", {a(0), a(1)}}},
        {"smin", {"smin", {a(0), a(1)}}},
        {"bfrev", {"bfrev", {a(0)}}},
        {"cbit", {"cbit", {a(0)}}},
        {"bfins", {"bfi", {a(0), a(1), a(2), a(3)}}},
        {"bfext", {"sbfe", {a(0), a(1), a(2)}}},
        {"fbl", {"fbl", {a(0)}}},
        {"sfbh", {"sfbh", {a(0)}}},
        {"ufbh", {"ufbh", {a(0)}}},
        {"inv", {"inv", {a(0)}}},
        {"log", {"log", {a(0)}}},
        {"exp", {"exp", {a(0)}}},
        {"sqrt", {"sqrt", {a(0)}}},
        {"sqrt_ieee", {"ieee.sqrt", {a(0)}}},
        {"rsqrt", {"rsqrt", {a(0)}}},
        {"sin", {"sin", {a(0)}}},
        {"cos", {"cos", {a(0)}}},
        {"pow", {"pow", {a(0), a(1)}}},
        {"div_ieee", {"ieee.div", {a(0), a(1)}}},
        {"dp4a", {"dp4a", {a(0), a(1), a(2)}}},
        {"any", {"any", {ai1(0)}}},
        {"all", {"all", {ai1(0)}}},
    };
  }

  const IntrinTable &getTable() { return Table; }
};

// The C++11 "magic static" idiom to lazily initialize the ESIMD intrinsic table
static const IntrinTable &getIntrinTable() {
  static ESIMDIntrinDescTable TheTable;
  return TheTable.getTable();
}

static const ESIMDIntrinDesc &getIntrinDesc(StringRef SrcSpelling) {
  static ESIMDIntrinDesc InvalidDesc{"", {}, {}};
  const auto &Table = getIntrinTable();
  auto It = Table.find(SrcSpelling.str());

  if (It == Table.end()) {
    Twine Msg("unknown ESIMD intrinsic: " + SrcSpelling);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  return It->second;
}

// Simplest possible implementation of an allocator for the Itanium demangler
class SimpleAllocator {
protected:
  SmallVector<void *, 128> Ptrs;

public:
  void reset() {
    for (void *Ptr : Ptrs) {
      // Destructors are not called, but that is OK for the
      // itanium_demangle::Node subclasses
      std::free(Ptr);
    }
    Ptrs.resize(0);
  }

  template <typename T, typename... Args> T *makeNode(Args &&... args) {
    void *Ptr = std::calloc(1, sizeof(T));
    Ptrs.push_back(Ptr);
    return new (Ptr) T(std::forward<Args>(args)...);
  }

  void *allocateNodeArray(size_t sz) {
    void *Ptr = std::calloc(sz, sizeof(id::Node *));
    Ptrs.push_back(Ptr);
    return Ptr;
  }

  ~SimpleAllocator() { reset(); }
};

Type *parsePrimitiveTypeString(StringRef TyStr, LLVMContext &Ctx) {
  return llvm::StringSwitch<Type *>(TyStr)
      .Case("bool", IntegerType::getInt1Ty(Ctx))
      .Case("char", IntegerType::getInt8Ty(Ctx))
      .Case("unsigned char", IntegerType::getInt8Ty(Ctx))
      .Case("short", IntegerType::getInt16Ty(Ctx))
      .Case("unsigned short", IntegerType::getInt16Ty(Ctx))
      .Case("int", IntegerType::getInt32Ty(Ctx))
      .Case("unsigned int", IntegerType::getInt32Ty(Ctx))
      .Case("unsigned", IntegerType::getInt32Ty(Ctx))
      .Case("unsigned long long", IntegerType::getInt64Ty(Ctx))
      .Case("long long", IntegerType::getInt64Ty(Ctx))
      .Case("float", IntegerType::getFloatTy(Ctx))
      .Case("double", IntegerType::getDoubleTy(Ctx))
      .Case("void", IntegerType::getVoidTy(Ctx))
      .Default(nullptr);
}

template <typename T>
static const T *castNodeImpl(const id::Node *N, id::Node::Kind K) {
  assert(N && N->getKind() == K && "unexpected demangler node kind");
  return reinterpret_cast<const T *>(N);
}

#define castNode(NodeObj, NodeKind)                                            \
  castNodeImpl<id::NodeKind>(NodeObj, id::Node::K##NodeKind)

static APInt parseTemplateArg(id::FunctionEncoding *FE, unsigned int N,
                              Type *&Ty, LLVMContext &Ctx) {
  const auto *Nm = castNode(FE->getName(), NameWithTemplateArgs);
  const auto *ArgsN = castNode(Nm->TemplateArgs, TemplateArgs);
  id::NodeArray Args = ArgsN->getParams();
  assert(N < Args.size() && "too few template arguments");
  id::StringView Val;

  switch (Args[N]->getKind()) {
  case id::Node::KIntegerLiteral: {
    auto *ValL = castNode(Args[N], IntegerLiteral);
    const id::StringView &TyStr = ValL->getType();
    Ty = TyStr.size() == 0 ? IntegerType::getInt32Ty(Ctx)
                           : parsePrimitiveTypeString(
                                 StringRef(TyStr.begin(), TyStr.size()), Ctx);
    Val = ValL->getValue();
    break;
  }
  case id::Node::KEnumLiteral: {
    auto *CE = castNode(Args[N], EnumLiteral);
    Ty = IntegerType::getInt32Ty(Ctx);
    Val = CE->getIntegerValue();
    break;
  }
  default:
    llvm_unreachable_internal("bad esimd intrinsic template parameter");
  }
  return APInt(Ty->getPrimitiveSizeInBits(), StringRef(Val.begin(), Val.size()),
               10);
}

// Constructs a GenX intrinsic name suffix based on the original C++ name (stem)
// and the types of its parameters (some intrinsic names have additional
// suffixes depending on the parameter types).
static std::string getESIMDIntrinSuffix(id::FunctionEncoding *FE,
                                        FunctionType *FT,
                                        const ESIMDIntrinDesc::NameRule &Rule) {
  std::string Suff;
  switch (Rule.Kind) {
  case ESIMDIntrinDesc::GenXSuffixRuleKind::BIN_OP: {
    // e.g. ".add"
    Type *Ty = nullptr;
    APInt OpId = parseTemplateArg(FE, Rule.I.TmplArgNo, Ty, FT->getContext());

    switch (OpId.getSExtValue()) {
    case 0x0:
      Suff = ".add";
      break;
    case 0x1:
      Suff = ".sub";
      break;
    case 0x2:
      Suff = ".inc";
      break;
    case 0x3:
      Suff = ".dec";
      break;
    case 0x4:
      Suff = ".min";
      break;
    case 0x5:
      Suff = ".max";
      break;
    case 0x6:
      Suff = ".xchg";
      break;
    case 0x7:
      Suff = ".cmpxchg";
      break;
    case 0x8:
      Suff = ".and";
      break;
    case 0x9:
      Suff = ".or";
      break;
    case 0xa:
      Suff = ".xor";
      break;
    case 0xb:
      Suff = ".minsint";
      break;
    case 0xc:
      Suff = ".maxsint";
      break;
    case 0x10:
      Suff = ".fmax";
      break;
    case 0x11:
      Suff = ".fmin";
      break;
    case 0x12:
      Suff = ".fcmpwr";
      break;
    case 0xff:
      Suff = ".predec";
      break;
    default:
      llvm_unreachable("unknown atomic OP");
    };
    break;
  }
  case ESIMDIntrinDesc::GenXSuffixRuleKind::NUM_KIND: {
    // e.g. "f"
    int No = Rule.I.CallArgNo;
    Type *Ty = No == -1 ? FT->getReturnType() : FT->getParamType(No);
    if (Ty->isVectorTy())
      Ty = cast<VectorType>(Ty)->getElementType();
    assert(Ty->isFloatingPointTy() || Ty->isIntegerTy());
    Suff = Ty->isFloatingPointTy() ? "f" : "i";
    break;
  }
  default:
    // It's ok if there is no suffix.
    break;
  }

  return Suff;
}

// Turn a MDNode into llvm::value or its subclass.
// Return nullptr if the underlying value has type mismatch.
template <typename Ty = llvm::Value> Ty *getVal(llvm::Metadata *M) {
  if (auto VM = dyn_cast<llvm::ValueAsMetadata>(M))
    if (auto V = dyn_cast<Ty>(VM->getValue()))
      return V;
  return nullptr;
}

/// Return the MDNode that has the SLM size attribute.
static llvm::MDNode *getSLMSizeMDNode(llvm::Function *F) {
  llvm::NamedMDNode *Nodes =
      F->getParent()->getNamedMetadata(GENX_KERNEL_METADATA);
  assert(Nodes && "invalid genx.kernels metadata");
  for (auto Node : Nodes->operands()) {
    if (Node->getNumOperands() >= 4 && getVal(Node->getOperand(0)) == F)
      return Node;
  }
  // if F is not a kernel, keep looking into its callers
  while (!F->use_empty()) {
    auto CI = cast<CallInst>(F->use_begin()->getUser());
    auto UF = CI->getParent()->getParent();
    if (auto Node = getSLMSizeMDNode(UF))
      return Node;
  }
  return nullptr;
}

static inline llvm::Metadata *getMD(llvm::Value *V) {
  return llvm::ValueAsMetadata::get(V);
}

static void translateSLMInit(CallInst &CI) {
  auto F = CI.getParent()->getParent();

  auto *ArgV = CI.getArgOperand(0);
  if (!isa<ConstantInt>(ArgV)) {
    assert(false && "integral constant expected for slm size");
    return;
  }
  auto NewVal = cast<llvm::ConstantInt>(ArgV)->getZExtValue();
  assert(NewVal != 0 && "zero slm bytes being requested");

  // find the corresponding kernel metadata and set the SLM size.
  if (llvm::MDNode *Node = getSLMSizeMDNode(F)) {
    if (llvm::Value *OldSz = getVal(Node->getOperand(4))) {
      assert(isa<llvm::ConstantInt>(OldSz) && "integer constant expected");
      llvm::Value *NewSz = llvm::ConstantInt::get(OldSz->getType(), NewVal);
      uint64_t OldVal = cast<llvm::ConstantInt>(OldSz)->getZExtValue();
      if (OldVal < NewVal)
        Node->replaceOperandWith(3, getMD(NewSz));
    }
  } else {
    // We check whether this call is inside a kernel function.
    assert(false && "slm_init shall be called by a kernel");
  }
}

static void translatePackMask(CallInst &CI) {
  using Demangler = id::ManglingParser<SimpleAllocator>;
  Function *F = CI.getCalledFunction();
  assert(F && "function to translate is invalid");

  StringRef MnglName = F->getName();
  Demangler Parser(MnglName.begin(), MnglName.end());
  id::Node *AST = Parser.parse();

  if (!AST || !Parser.ForwardTemplateRefs.empty()) {
    Twine Msg("failed to demangle ESIMD intrinsic: " + MnglName);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  if (AST->getKind() != id::Node::KFunctionEncoding) {
    Twine Msg("bad ESIMD intrinsic: " + MnglName);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  auto *FE = static_cast<id::FunctionEncoding *>(AST);
  llvm::LLVMContext &Context = CI.getContext();
  Type *TTy = nullptr;
  APInt Val = parseTemplateArg(FE, 0, TTy, Context);
  unsigned N = Val.getZExtValue();

  IRBuilder<> Builder(&CI);
  llvm::Value *Trunc = Builder.CreateTrunc(
      CI.getArgOperand(0),
      llvm::FixedVectorType::get(llvm::Type::getInt1Ty(Context), N));
  llvm::Type *Ty = llvm::Type::getIntNTy(Context, N);

  llvm::Value *BitCast = Builder.CreateBitCast(Trunc, Ty);
  llvm::Value *Result = BitCast;
  if (N != 32) {
    Result = Builder.CreateCast(llvm::Instruction::ZExt, BitCast,
                                llvm::Type::getInt32Ty(Context));
  }

  Result->setName(CI.getName());
  cast<llvm::Instruction>(Result)->setDebugLoc(CI.getDebugLoc());
  CI.replaceAllUsesWith(Result);
}

static void translateUnPackMask(CallInst &CI) {
  using Demangler = id::ManglingParser<SimpleAllocator>;
  Function *F = CI.getCalledFunction();
  assert(F && "function to translate is invalid");
  StringRef MnglName = F->getName();
  Demangler Parser(MnglName.begin(), MnglName.end());
  id::Node *AST = Parser.parse();

  if (!AST || !Parser.ForwardTemplateRefs.empty()) {
    Twine Msg("failed to demangle ESIMD intrinsic: " + MnglName);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  if (AST->getKind() != id::Node::KFunctionEncoding) {
    Twine Msg("bad ESIMD intrinsic: " + MnglName);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  auto *FE = static_cast<id::FunctionEncoding *>(AST);
  llvm::LLVMContext &Context = CI.getContext();
  Type *TTy = nullptr;
  APInt Val = parseTemplateArg(FE, 0, TTy, Context);
  unsigned N = Val.getZExtValue();
  // get N x i1
  assert(CI.getNumArgOperands() == 1);
  llvm::Value *Arg0 = CI.getArgOperand(0);
  unsigned Width = Arg0->getType()->getPrimitiveSizeInBits();
  IRBuilder<> Builder(&CI);
  if (Width > N) {
    llvm::Type *Ty = llvm::IntegerType::get(Context, N);
    Arg0 = Builder.CreateTrunc(Arg0, Ty);
    cast<llvm::Instruction>(Arg0)->setDebugLoc(CI.getDebugLoc());
  }
  assert(Arg0->getType()->getPrimitiveSizeInBits() == N);
  Arg0 = Builder.CreateBitCast(
      Arg0, llvm::FixedVectorType::get(llvm::Type::getInt1Ty(Context), N));

  // get N x i16
  llvm::Value *TransCI = Builder.CreateZExt(
      Arg0, llvm::FixedVectorType::get(llvm::Type::getInt16Ty(Context), N));
  TransCI->takeName(&CI);
  cast<llvm::Instruction>(TransCI)->setDebugLoc(CI.getDebugLoc());
  CI.replaceAllUsesWith(TransCI);
}

static bool translateVLoad(CallInst &CI, SmallPtrSet<Type *, 4> &GVTS) {
  if (GVTS.find(CI.getType()) != GVTS.end())
    return false;
  IRBuilder<> Builder(&CI);
  auto LI = Builder.CreateLoad(CI.getArgOperand(0), CI.getName());
  LI->setDebugLoc(CI.getDebugLoc());
  CI.replaceAllUsesWith(LI);
  return true;
}

static bool translateVStore(CallInst &CI, SmallPtrSet<Type *, 4> &GVTS) {
  if (GVTS.find(CI.getOperand(1)->getType()) != GVTS.end())
    return false;
  IRBuilder<> Builder(&CI);
  auto SI = Builder.CreateStore(CI.getArgOperand(1), CI.getArgOperand(0));
  SI->setDebugLoc(CI.getDebugLoc());
  return true;
}

static void translateGetValue(CallInst &CI) {
  auto opnd = CI.getArgOperand(0);
  assert(opnd->getType()->isPointerTy());
  IRBuilder<> Builder(&CI);
  auto SV =
      Builder.CreatePtrToInt(opnd, IntegerType::getInt32Ty(CI.getContext()));
  auto *SI = cast<CastInst>(SV);
  SI->setDebugLoc(CI.getDebugLoc());
  CI.replaceAllUsesWith(SI);
}

// Newly created GenX intrinsic might have different return type than expected.
// This helper function creates cast operation from GenX intrinsic return type
// to currently expected. Returns pointer to created cast instruction if it
// was created, otherwise returns NewI.
static Instruction *addCastInstIfNeeded(Instruction *OldI, Instruction *NewI) {
  Type *NITy = NewI->getType();
  Type *OITy = OldI->getType();
  if (OITy != NITy) {
    auto CastOpcode = CastInst::getCastOpcode(NewI, false, OITy, false);
    NewI = CastInst::Create(CastOpcode, NewI, OITy,
                            NewI->getName() + ".cast.ty", OldI);
  }
  return NewI;
}

static int getIndexForSuffix(StringRef Suff) {
  return llvm::StringSwitch<int>(Suff)
      .Case("x", 0)
      .Case("y", 1)
      .Case("z", 2)
      .Default(-1);
}

// Helper function to convert SPIRV intrinsic into GenX intrinsic,
// that returns vector of coordinates.
// Example:
//   %call = call spir_func i64 @_Z23__spirv_WorkgroupSize_xv()
//     =>
//   %call.esimd = tail call <3 x i32> @llvm.genx.local.size.v3i32()
//   %wgsize.x = extractelement <3 x i32> %call.esimd, i32 0
//   %wgsize.x.cast.ty = zext i32 %wgsize.x to i64
static Instruction *generateVectorGenXForSpirv(CallInst &CI, StringRef Suff,
                                               const std::string &IntrinName,
                                               StringRef ValueName) {
  std::string IntrName =
      std::string(GenXIntrinsic::getGenXIntrinsicPrefix()) + IntrinName;
  auto ID = GenXIntrinsic::lookupGenXIntrinsicID(IntrName);
  LLVMContext &Ctx = CI.getModule()->getContext();
  Type *I32Ty = Type::getInt32Ty(Ctx);
  Function *NewFDecl = GenXIntrinsic::getGenXDeclaration(
      CI.getModule(), ID, {FixedVectorType::get(I32Ty, 3)});
  Instruction *IntrI =
      IntrinsicInst::Create(NewFDecl, {}, CI.getName() + ".esimd", &CI);
  int ExtractIndex = getIndexForSuffix(Suff);
  assert(ExtractIndex != -1 && "Extract index is invalid.");
  Twine ExtractName = ValueName + Suff;
  Instruction *ExtrI = ExtractElementInst::Create(
      IntrI, ConstantInt::get(I32Ty, ExtractIndex), ExtractName, &CI);
  Instruction *CastI = addCastInstIfNeeded(&CI, ExtrI);
  return CastI;
}

// Helper function to convert SPIRV intrinsic into GenX intrinsic,
// that has exact mapping.
// Example:
//   %call = call spir_func i64 @_Z21__spirv_WorkgroupId_xv()
//     =>
//   %group.id.x = tail call i32 @llvm.genx.group.id.x()
//   %group.id.x.cast.ty = zext i32 %group.id.x to i64
static Instruction *generateGenXForSpirv(CallInst &CI, StringRef Suff,
                                         const std::string &IntrinName) {
  std::string IntrName = std::string(GenXIntrinsic::getGenXIntrinsicPrefix()) +
                         IntrinName + Suff.str();
  auto ID = GenXIntrinsic::lookupGenXIntrinsicID(IntrName);
  Function *NewFDecl =
      GenXIntrinsic::getGenXDeclaration(CI.getModule(), ID, {});
  Instruction *IntrI =
      IntrinsicInst::Create(NewFDecl, {}, IntrinName + Suff.str(), &CI);
  Instruction *CastI = addCastInstIfNeeded(&CI, IntrI);
  return CastI;
}

// This function translates SPIRV intrinsic into GenX intrinsic.
// TODO: Currently, we do not support mixing SYCL and ESIMD kernels.
// Later for ESIMD and SYCL kernels to coexist, we likely need to
// clone call graph that lead from ESIMD kernel to SPIRV intrinsic and
// translate SPIRV intrinsics to GenX intrinsics only in cloned subgraph.
static void
translateSpirvIntrinsic(CallInst *CI, StringRef SpirvIntrName,
                        SmallVector<Instruction *, 8> &ESIMDToErases) {
  auto translateSpirvIntr = [&SpirvIntrName, &ESIMDToErases,
                             CI](StringRef SpvIName, auto TranslateFunc) {
    if (SpirvIntrName.consume_front(SpvIName)) {
      Value *TranslatedV = TranslateFunc(*CI, SpirvIntrName.substr(1, 1));
      CI->replaceAllUsesWith(TranslatedV);
      ESIMDToErases.push_back(CI);
    }
  };

  translateSpirvIntr("WorkgroupSize", [](CallInst &CI, StringRef Suff) {
    return generateVectorGenXForSpirv(CI, Suff, "local.size.v3i32", "wgsize.");
  });
  translateSpirvIntr("LocalInvocationId", [](CallInst &CI, StringRef Suff) {
    return generateVectorGenXForSpirv(CI, Suff, "local.id.v3i32", "local_id.");
  });
  translateSpirvIntr("WorkgroupId", [](CallInst &CI, StringRef Suff) {
    return generateGenXForSpirv(CI, Suff, "group.id.");
  });
  translateSpirvIntr("GlobalInvocationId", [](CallInst &CI, StringRef Suff) {
    // GlobalId = LocalId + WorkGroupSize * GroupId
    Instruction *LocalIdI =
        generateVectorGenXForSpirv(CI, Suff, "local.id.v3i32", "local_id.");
    Instruction *WGSizeI =
        generateVectorGenXForSpirv(CI, Suff, "local.size.v3i32", "wgsize.");
    Instruction *GroupIdI = generateGenXForSpirv(CI, Suff, "group.id.");
    Instruction *MulI =
        BinaryOperator::CreateMul(WGSizeI, GroupIdI, "mul", &CI);
    return BinaryOperator::CreateAdd(LocalIdI, MulI, "add", &CI);
  });
  translateSpirvIntr("GlobalSize", [](CallInst &CI, StringRef Suff) {
    // GlobalSize = WorkGroupSize * NumWorkGroups
    Instruction *WGSizeI =
        generateVectorGenXForSpirv(CI, Suff, "local.size.v3i32", "wgsize.");
    Instruction *NumWGI = generateVectorGenXForSpirv(
        CI, Suff, "group.count.v3i32", "group_count.");
    return BinaryOperator::CreateMul(WGSizeI, NumWGI, "mul", &CI);
  });
  // TODO: Support GlobalOffset SPIRV intrinsics
  translateSpirvIntr("GlobalOffset", [](CallInst &CI, StringRef Suff) {
    return llvm::Constant::getNullValue(CI.getType());
  });
  translateSpirvIntr("NumWorkgroups", [](CallInst &CI, StringRef Suff) {
    return generateVectorGenXForSpirv(CI, Suff, "group.count.v3i32",
                                      "group_count.");
  });
}

static void createESIMDIntrinsicArgs(const ESIMDIntrinDesc &Desc,
                                     SmallVector<Value *, 16> &GenXArgs,
                                     CallInst &CI, id::FunctionEncoding *FE) {
  uint32_t LastCppArgNo = 0; // to implement SRC_CALL_ALL

  for (unsigned int I = 0; I < Desc.ArgRules.size(); ++I) {
    const ESIMDIntrinDesc::ArgRule &Rule = Desc.ArgRules[I];

    switch (Rule.Kind) {
    case ESIMDIntrinDesc::GenXArgRuleKind::SRC_CALL_ARG: {
      Value *Arg = CI.getArgOperand(Rule.I.Arg.CallArgNo);

      switch (Rule.I.Arg.Conv) {
      case ESIMDIntrinDesc::GenXArgConversion::NONE:
        GenXArgs.push_back(Arg);
        break;
      case ESIMDIntrinDesc::GenXArgConversion::TO_I1: {
        // convert N-bit integer to 1-bit integer
        Type *NTy = Arg->getType();
        assert(NTy->isIntOrIntVectorTy());
        Value *Zero = ConstantInt::get(NTy, 0);
        IRBuilder<> Bld(&CI);
        auto *Cmp = Bld.CreateICmp(ICmpInst::ICMP_NE, Arg, Zero);
        GenXArgs.push_back(Cmp);
        break;
      }
      case ESIMDIntrinDesc::GenXArgConversion::TO_SI: {
        // convert a pointer to 32-bit integer surface index
        assert(Arg->getType()->isPointerTy());
        IRBuilder<> Bld(&CI);
        Value *Res =
            Bld.CreatePtrToInt(Arg, IntegerType::getInt32Ty(CI.getContext()));
        GenXArgs.push_back(Res);
        break;
      }
      default:
        llvm_unreachable("Unknown ESIMD arg conversion");
      }
      LastCppArgNo = Rule.I.Arg.CallArgNo;
      break;
    }
    case ESIMDIntrinDesc::GenXArgRuleKind::SRC_CALL_ALL:
      assert(LastCppArgNo < CI.getNumArgOperands());
      for (uint32_t N = LastCppArgNo; N < CI.getNumArgOperands(); ++N)
        GenXArgs.push_back(CI.getArgOperand(N));
      break;
    case ESIMDIntrinDesc::GenXArgRuleKind::SRC_TMPL_ARG: {
      Type *Ty = nullptr;
      APInt Val = parseTemplateArg(FE, Rule.I.TmplArgNo, Ty, CI.getContext());
      Value *ArgVal = ConstantInt::get(
          Ty, static_cast<uint64_t>(Val.getSExtValue()), true /*signed*/);
      GenXArgs.push_back(ArgVal);
      break;
    }
    case ESIMDIntrinDesc::GenXArgRuleKind::NUM_BYTES: {
      Type *Ty = Rule.I.Arg.CallArgNo == -1
                     ? CI.getType()
                     : CI.getArgOperand(Rule.I.Arg.CallArgNo)->getType();
      assert(Ty->isVectorTy());
      int NBits =
          cast<VectorType>(Ty)->getElementType()->getPrimitiveSizeInBits();
      assert(NBits == 8 || NBits == 16 || NBits == 32);
      int NWords = NBits / 16;
      GenXArgs.push_back(
          ConstantInt::get(IntegerType::getInt32Ty(CI.getContext()), NWords));
      break;
    }
    case ESIMDIntrinDesc::GenXArgRuleKind::UNDEF: {
      Type *Ty = Rule.I.Arg.CallArgNo == -1
                     ? CI.getType()
                     : CI.getArgOperand(Rule.I.Arg.CallArgNo)->getType();
      GenXArgs.push_back(UndefValue::get(Ty));
      break;
    }
    case ESIMDIntrinDesc::GenXArgRuleKind::CONST_INT16: {
      auto Ty = IntegerType::getInt16Ty(CI.getContext());
      GenXArgs.push_back(llvm::ConstantInt::get(Ty, Rule.I.ArgConst));
      break;
    }
    case ESIMDIntrinDesc::GenXArgRuleKind::CONST_INT32: {
      auto Ty = IntegerType::getInt32Ty(CI.getContext());
      GenXArgs.push_back(llvm::ConstantInt::get(Ty, Rule.I.ArgConst));
      break;
    }
    case ESIMDIntrinDesc::GenXArgRuleKind::CONST_INT64: {
      auto Ty = IntegerType::getInt64Ty(CI.getContext());
      GenXArgs.push_back(llvm::ConstantInt::get(Ty, Rule.I.ArgConst));
      break;
    }
    default:
      llvm_unreachable_internal("unknown argument rule kind");
    }
  }
}

// Demangles and translates given ESIMD intrinsic call instruction. Example
//
// ### Source-level intrinsic:
//
// sycl::intel::gpu::__vector_type<int, 16>::type __esimd_flat_read<int, 16>(
//     sycl::intel::gpu::__vector_type<unsigned long long, 16>::type,
//     sycl::intel::gpu::__vector_type<int, 16>::type)
//
// ### Itanium-mangled name:
//
// _Z14__esimd_flat_readIiLi16EEN2cm3gen13__vector_typeIT_XT0_EE4typeENS2_IyXT0_EE4typeES5_
//
// ### Itanium demangler IR:
//
// FunctionEncoding(
//  NestedName(
//    NameWithTemplateArgs(
//      NestedName(
//        NestedName(
//          NameType("cm"),
//          NameType("gen")),
//        NameType("__vector_type")),
//      TemplateArgs(
//        {NameType("int"),
//         IntegerLiteral("", "16")})),
//    NameType("type")),
//  NameWithTemplateArgs(
//    NameType("__esimd_flat_read"),
//    TemplateArgs(
//      {NameType("int"),
//       IntegerLiteral("", "16")})),
//  {NestedName(
//     NameWithTemplateArgs(
//       NestedName(
//         NestedName(
//           NameType("cm"),
//           NameType("gen")),
//         NameType("__vector_type")),
//       TemplateArgs(
//         {NameType("unsigned long long"),
//          IntegerLiteral("", "16")})),
//     NameType("type")),
//   NestedName(
//     NameWithTemplateArgs(
//       NestedName(
//         NestedName(
//           NameType("cm"),
//           NameType("gen")),
//         NameType("__vector_type")),
//       TemplateArgs(
//         {NameType("int"),
//          IntegerLiteral("", "16")})),
//     NameType("type"))},
//  <null>,
//  QualNone, FunctionRefQual::FrefQualNone)
//
static void translateESIMDIntrinsicCall(CallInst &CI) {
  using Demangler = id::ManglingParser<SimpleAllocator>;
  Function *F = CI.getCalledFunction();
  assert(F && "function to translate is invalid");
  StringRef MnglName = F->getName();
  Demangler Parser(MnglName.begin(), MnglName.end());
  id::Node *AST = Parser.parse();

  if (!AST || !Parser.ForwardTemplateRefs.empty()) {
    Twine Msg("failed to demangle ESIMD intrinsic: " + MnglName);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  if (AST->getKind() != id::Node::KFunctionEncoding) {
    Twine Msg("bad ESIMD intrinsic: " + MnglName);
    llvm::report_fatal_error(Msg, false /*no crash diag*/);
  }
  auto *FE = static_cast<id::FunctionEncoding *>(AST);
  id::StringView BaseNameV = FE->getName()->getBaseName();

  auto PrefLen = StringRef(ESIMD_INTRIN_PREF1).size();
  StringRef BaseName(BaseNameV.begin() + PrefLen, BaseNameV.size() - PrefLen);
  const auto &Desc = getIntrinDesc(BaseName);
  if (!Desc.isValid()) // TODO remove this once all intrinsics are supported
    return;

  auto *FTy = F->getFunctionType();
  std::string Suffix = getESIMDIntrinSuffix(FE, FTy, Desc.SuffixRule);
  auto ID = GenXIntrinsic::lookupGenXIntrinsicID(
      GenXIntrinsic::getGenXIntrinsicPrefix() + Desc.GenXSpelling + Suffix);

  SmallVector<Value *, 16> GenXArgs;
  createESIMDIntrinsicArgs(Desc, GenXArgs, CI, FE);

  SmallVector<Type *, 16> GenXOverloadedTypes;
  if (GenXIntrinsic::isOverloadedRet(ID))
    GenXOverloadedTypes.push_back(CI.getType());
  for (unsigned i = 0; i < GenXArgs.size(); ++i)
    if (GenXIntrinsic::isOverloadedArg(ID, i))
      GenXOverloadedTypes.push_back(GenXArgs[i]->getType());

  Function *NewFDecl = GenXIntrinsic::getGenXDeclaration(CI.getModule(), ID,
                                                         GenXOverloadedTypes);

  Instruction *NewCI = IntrinsicInst::Create(
      NewFDecl, GenXArgs,
      NewFDecl->getReturnType()->isVoidTy() ? "" : CI.getName() + ".esimd",
      &CI);
  NewCI = addCastInstIfNeeded(&CI, NewCI);
  CI.replaceAllUsesWith(NewCI);
  CI.eraseFromParent();
}

static std::string getMDString(MDNode *N, unsigned I) {
  if (!N)
    return "";

  Metadata *Op = N->getOperand(I);
  if (!Op)
    return "";

  if (MDString *Str = dyn_cast<MDString>(Op)) {
    return Str->getString().str();
  }

  return "";
}

void SYCLLowerESIMDLegacyPass::generateKernelMetadata(Module &M) {
  if (M.getNamedMetadata(GENX_KERNEL_METADATA))
    return;

  auto Kernels = M.getOrInsertNamedMetadata(GENX_KERNEL_METADATA);
  assert(Kernels->getNumOperands() == 0 && "metadata out of sync");

  LLVMContext &Ctx = M.getContext();
  Type *I32Ty = Type::getInt32Ty(Ctx);

  std::string TargetTriple = M.getTargetTriple();
  llvm::Triple T(TargetTriple);
  T.setArchName("genx64");
  TargetTriple = T.str();
  M.setTargetTriple(TargetTriple);

  enum { AK_NORMAL, AK_SAMPLER, AK_SURFACE, AK_VME };
  enum { IK_NORMAL, IK_INPUT, IK_OUTPUT, IK_INPUT_OUTPUT };

  for (auto &F : M.functions()) {
    // Skip non-SIMD kernels.
    if (F.getCallingConv() != CallingConv::SPIR_KERNEL ||
        F.getMetadata("sycl_explicit_simd") == nullptr)
      continue;

    // Metadata node containing N i32s, where N is the number of kernel
    // arguments, and each i32 is the kind of argument,  one of:
    //     0 = general, 1 = sampler, 2 = surface, 3 = vme
    // (the same values as in the "kind" field of an "input_info" record in a
    // vISA kernel.
    SmallVector<Metadata *, 8> ArgKinds;

    // Optional, not supported for compute
    SmallVector<Metadata *, 8> ArgInOutKinds;

    // Metadata node describing N strings where N is the number of kernel
    // arguments, each string describing argument type in OpenCL.
    // required for running on top of OpenCL runtime.
    SmallVector<Metadata *, 8> ArgTypeDescs;

    auto *KernelArgTypes = F.getMetadata("kernel_arg_type");
    auto *KernelArgAccPtrs = F.getMetadata("kernel_arg_accessor_ptr");
    unsigned Idx = 0;

    // Iterate argument list to gather argument kinds and generate argument
    // descriptors.
    for (const Argument &Arg : F.args()) {
      int Kind = AK_NORMAL;
      int IKind = IK_NORMAL;

      auto ArgType = getMDString(KernelArgTypes, Idx);

      if (ArgType.find("image1d_t") != std::string::npos ||
          ArgType.find("image2d_t") != std::string::npos ||
          ArgType.find("image3d_t") != std::string::npos) {
        Kind = AK_SURFACE;
        ArgTypeDescs.push_back(MDString::get(Ctx, ArgType));
      } else {
        StringRef ArgDesc = "";

        if (Arg.getType()->isPointerTy()) {
          const auto *IsAccMD =
              KernelArgAccPtrs
                  ? cast<ConstantAsMetadata>(KernelArgAccPtrs->getOperand(Idx))
                  : nullptr;
          unsigned IsAcc =
              IsAccMD
                  ? static_cast<unsigned>(cast<ConstantInt>(IsAccMD->getValue())
                                              ->getValue()
                                              .getZExtValue())
                  : 0;
          if (IsAcc) {
            ArgDesc = "buffer_t";
            Kind = AK_SURFACE;
          } else
            ArgDesc = "svmptr_t";
        }
        ArgTypeDescs.push_back(MDString::get(Ctx, ArgDesc));
      }

      ArgKinds.push_back(getMD(ConstantInt::get(I32Ty, Kind)));
      ArgInOutKinds.push_back(getMD(ConstantInt::get(I32Ty, IKind)));

      Idx++;
    }

    MDNode *Kinds = MDNode::get(Ctx, ArgKinds);
    MDNode *IOKinds = MDNode::get(Ctx, ArgInOutKinds);
    MDNode *ArgDescs = MDNode::get(Ctx, ArgTypeDescs);

    Metadata *MDArgs[] = {
        getMD(&F),
        MDString::get(Ctx, F.getName().str()),
        Kinds,
        getMD(llvm::ConstantInt::getNullValue(I32Ty)), // SLM size in bytes
        getMD(llvm::ConstantInt::getNullValue(I32Ty)), // arg offsets
        IOKinds,
        ArgDescs};

    // Add this kernel to the root.
    Kernels->addOperand(MDNode::get(Ctx, MDArgs));
    F.addFnAttr("oclrt", "1");
    F.addFnAttr("CMGenxMain");
  }
}

// collect all the vector-types that are used by genx-volatiles
void SYCLLowerESIMDLegacyPass::collectGenXVolatileType(Module &M) {
  for (auto &G : M.getGlobalList()) {
    if (!G.hasAttribute("genx_volatile"))
      continue;
    auto PTy = dyn_cast<PointerType>(G.getType());
    if (!PTy)
      continue;
    auto GTy = dyn_cast<StructType>(PTy->getPointerElementType());
    if (!GTy || !GTy->getName().endswith("cl::sycl::INTEL::gpu::simd"))
      continue;
    assert(GTy->getNumContainedTypes() == 1);
    auto VTy = GTy->getContainedType(0);
    assert(VTy->isVectorTy());
    GenXVolatileTypeSet.insert(VTy);
  }
}

} // namespace

PreservedAnalyses SYCLLowerESIMDPass::run(Function &F,
                                          FunctionAnalysisManager &FAM,
                                          SmallPtrSet<Type *, 4> &GVTS) {
  // Only consider functions marked with !sycl_explicit_simd
  if (F.getMetadata("sycl_explicit_simd") == nullptr)
    return PreservedAnalyses::all();

  SmallVector<CallInst *, 32> ESIMDIntrCalls;
  SmallVector<Instruction *, 8> ESIMDToErases;

  for (Instruction &I : instructions(F)) {
    if (auto CastOp = dyn_cast<llvm::CastInst>(&I)) {
      llvm::Type *DstTy = CastOp->getDestTy();
      auto CastOpcode = CastOp->getOpcode();
      if ((CastOpcode == llvm::Instruction::FPToUI &&
           DstTy->getScalarType()->getPrimitiveSizeInBits() <= 32) ||
          (CastOpcode == llvm::Instruction::FPToSI &&
           DstTy->getScalarType()->getPrimitiveSizeInBits() < 32)) {
        IRBuilder<> Builder(&I);
        llvm::Value *Src = CastOp->getOperand(0);
        auto TmpTy = llvm::FixedVectorType::get(
            llvm::Type::getInt32Ty(DstTy->getContext()),
            cast<FixedVectorType>(DstTy)->getNumElements());
        Src = Builder.CreateFPToSI(Src, TmpTy);

        llvm::Instruction::CastOps TruncOp = llvm::Instruction::Trunc;
        llvm::Value *NewDst = Builder.CreateCast(TruncOp, Src, DstTy);
        CastOp->replaceAllUsesWith(NewDst);
        ESIMDToErases.push_back(CastOp);
      }
    }

    auto *CI = dyn_cast<CallInst>(&I);
    Function *Callee = nullptr;
    if (!CI || !(Callee = CI->getCalledFunction()))
      continue;
    StringRef Name = Callee->getName();

    // See if the Name represents an ESIMD intrinsic and demangle only if it
    // does.
    if (!Name.consume_front(ESIMD_INTRIN_PREF0))
      continue;
    // now skip the digits
    Name = Name.drop_while([](char C) { return std::isdigit(C); });

    // process ESIMD builtins that go through special handling instead of
    // the translation procedure
    if (Name.startswith("N2cl4sycl5INTEL3gpu8slm_init")) {
      // tag the kernel with meta-data SLMSize, and remove this builtin
      translateSLMInit(*CI);
      ESIMDToErases.push_back(CI);
      continue;
    }
    if (Name.startswith("__esimd_pack_mask")) {
      translatePackMask(*CI);
      ESIMDToErases.push_back(CI);
      continue;
    }
    if (Name.startswith("__esimd_unpack_mask")) {
      translateUnPackMask(*CI);
      ESIMDToErases.push_back(CI);
      continue;
    }
    // If vload/vstore is not about the vector-types used by
    // those globals marked as genx_volatile, We can translate
    // them directly into generic load/store inst. In this way
    // those insts can be optimized by llvm ASAP.
    if (Name.startswith("__esimd_vload")) {
      if (translateVLoad(*CI, GVTS)) {
        ESIMDToErases.push_back(CI);
        continue;
      }
    }
    if (Name.startswith("__esimd_vstore")) {
      if (translateVStore(*CI, GVTS)) {
        ESIMDToErases.push_back(CI);
        continue;
      }
    }

    if (Name.startswith("__esimd_get_value")) {
      translateGetValue(*CI);
      ESIMDToErases.push_back(CI);
      continue;
    }

    if (Name.consume_front(SPIRV_INTRIN_PREF)) {
      translateSpirvIntrinsic(CI, Name, ESIMDToErases);
      // For now: if no match, just let it go untranslated.
      continue;
    }

    if (Name.empty() || !Name.startswith(ESIMD_INTRIN_PREF1))
      continue;
    // this is ESIMD intrinsic - record for later translation
    ESIMDIntrCalls.push_back(CI);
  }
  // Now demangle and translate found ESIMD intrinsic calls
  for (auto *CI : ESIMDIntrCalls) {
    translateESIMDIntrinsicCall(*CI);
  }
  for (auto *CI : ESIMDToErases) {
    CI->eraseFromParent();
  }

  // TODO FIXME ESIMD figure out less conservative result
  return ESIMDIntrCalls.size() > 0 ? PreservedAnalyses::none()
                                   : PreservedAnalyses::all();
}
