//===- SemaSYCL.cpp - Semantic Analysis for SYCL constructs ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This implements Semantic Analysis for SYCL constructs.
//===----------------------------------------------------------------------===//

#include "TreeTransform.h"
#include "clang/AST/AST.h"
#include "clang/AST/Mangle.h"
#include "clang/AST/QualTypeNames.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/TemplateArgumentVisitor.h"
#include "clang/AST/TypeVisitor.h"
#include "clang/Analysis/CallGraph.h"
#include "clang/Basic/Attributes.h"
#include "clang/Basic/Builtins.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Sema/Initialization.h"
#include "clang/Sema/Sema.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <array>
#include <functional>
#include <initializer_list>

using namespace clang;
using namespace std::placeholders;

using KernelParamKind = SYCLIntegrationHeader::kernel_param_kind_t;

enum target {
  global_buffer = 2014,
  constant_buffer,
  local,
  image,
  host_buffer,
  host_image,
  image_array
};

using ParamDesc = std::tuple<QualType, IdentifierInfo *, TypeSourceInfo *>;

enum KernelInvocationKind {
  InvokeUnknown,
  InvokeSingleTask,
  InvokeParallelFor,
  InvokeParallelForWorkGroup
};

const static std::string InitMethodName = "__init";
const static std::string InitESIMDMethodName = "__init_esimd";
const static std::string FinalizeMethodName = "__finalize";
constexpr unsigned MaxKernelArgsSize = 2048;

namespace {

/// Various utilities.
class Util {
public:
  using DeclContextDesc = std::pair<clang::Decl::Kind, StringRef>;

  /// Checks whether given clang type is a full specialization of the SYCL
  /// accessor class.
  static bool isSyclAccessorType(const QualType &Ty);

  /// Checks whether given clang type is a full specialization of the SYCL
  /// sampler class.
  static bool isSyclSamplerType(const QualType &Ty);

  /// Checks whether given clang type is a full specialization of the SYCL
  /// stream class.
  static bool isSyclStreamType(const QualType &Ty);

  /// Checks whether given clang type is a full specialization of the SYCL
  /// half class.
  static bool isSyclHalfType(const QualType &Ty);

  /// Checks whether given clang type is a full specialization of the SYCL
  /// accessor_property_list class.
  static bool isAccessorPropertyListType(const QualType &Ty);

  /// Checks whether given clang type is a full specialization of the SYCL
  /// buffer_location class.
  static bool isSyclBufferLocationType(const QualType &Ty);

  /// Checks whether given clang type is a standard SYCL API class with given
  /// name.
  /// \param Ty    the clang type being checked
  /// \param Name  the class name checked against
  /// \param Tmpl  whether the class is template instantiation or simple record
  static bool isSyclType(const QualType &Ty, StringRef Name, bool Tmpl = false);

  /// Checks whether given function is a standard SYCL API function with given
  /// name.
  /// \param FD    the function being checked.
  /// \param Name  the function name to be checked against.
  static bool isSyclFunction(const FunctionDecl *FD, StringRef Name);

  /// Checks whether given clang type is a full specialization of the SYCL
  /// specialization constant class.
  static bool isSyclSpecConstantType(const QualType &Ty);

  // Checks declaration context hierarchy.
  /// \param DC     the context of the item to be checked.
  /// \param Scopes the declaration scopes leading from the item context to the
  ///               translation unit (excluding the latter)
  static bool matchContext(const DeclContext *DC,
                           ArrayRef<Util::DeclContextDesc> Scopes);

  /// Checks whether given clang type is declared in the given hierarchy of
  /// declaration contexts.
  /// \param Ty         the clang type being checked
  /// \param Scopes     the declaration scopes leading from the type to the
  ///     translation unit (excluding the latter)
  static bool matchQualifiedTypeName(const QualType &Ty,
                                     ArrayRef<Util::DeclContextDesc> Scopes);
};

} // anonymous namespace

// This information is from Section 4.13 of the SYCL spec
// https://www.khronos.org/registry/SYCL/specs/sycl-1.2.1.pdf
// This function returns false if the math lib function
// corresponding to the input builtin is not supported
// for SYCL
static bool IsSyclMathFunc(unsigned BuiltinID) {
  switch (BuiltinID) {
  case Builtin::BIlround:
  case Builtin::BI__builtin_lround:
  case Builtin::BIceill:
  case Builtin::BI__builtin_ceill:
  case Builtin::BIcopysignl:
  case Builtin::BI__builtin_copysignl:
  case Builtin::BIcosl:
  case Builtin::BI__builtin_cosl:
  case Builtin::BIexpl:
  case Builtin::BI__builtin_expl:
  case Builtin::BIexp2l:
  case Builtin::BI__builtin_exp2l:
  case Builtin::BIfabsl:
  case Builtin::BI__builtin_fabsl:
  case Builtin::BIfloorl:
  case Builtin::BI__builtin_floorl:
  case Builtin::BIfmal:
  case Builtin::BI__builtin_fmal:
  case Builtin::BIfmaxl:
  case Builtin::BI__builtin_fmaxl:
  case Builtin::BIfminl:
  case Builtin::BI__builtin_fminl:
  case Builtin::BIfmodl:
  case Builtin::BI__builtin_fmodl:
  case Builtin::BIlogl:
  case Builtin::BI__builtin_logl:
  case Builtin::BIlog10l:
  case Builtin::BI__builtin_log10l:
  case Builtin::BIlog2l:
  case Builtin::BI__builtin_log2l:
  case Builtin::BIpowl:
  case Builtin::BI__builtin_powl:
  case Builtin::BIrintl:
  case Builtin::BI__builtin_rintl:
  case Builtin::BIroundl:
  case Builtin::BI__builtin_roundl:
  case Builtin::BIsinl:
  case Builtin::BI__builtin_sinl:
  case Builtin::BIsqrtl:
  case Builtin::BI__builtin_sqrtl:
  case Builtin::BItruncl:
  case Builtin::BI__builtin_truncl:
  case Builtin::BIlroundl:
  case Builtin::BI__builtin_lroundl:
  case Builtin::BIfmax:
  case Builtin::BI__builtin_fmax:
  case Builtin::BIfmin:
  case Builtin::BI__builtin_fmin:
  case Builtin::BIfmaxf:
  case Builtin::BI__builtin_fmaxf:
  case Builtin::BIfminf:
  case Builtin::BI__builtin_fminf:
  case Builtin::BIlroundf:
  case Builtin::BI__builtin_lroundf:
  case Builtin::BI__builtin_fpclassify:
  case Builtin::BI__builtin_isfinite:
  case Builtin::BI__builtin_isinf:
  case Builtin::BI__builtin_isnormal:
    return false;
  default:
    break;
  }
  return true;
}

bool Sema::isKnownGoodSYCLDecl(const Decl *D) {
  if (const FunctionDecl *FD = dyn_cast<FunctionDecl>(D)) {
    const IdentifierInfo *II = FD->getIdentifier();
    const DeclContext *DC = FD->getDeclContext();
    if (II && II->isStr("__spirv_ocl_printf") &&
        !FD->isDefined() &&
        FD->getLanguageLinkage() == CXXLanguageLinkage &&
        DC->getEnclosingNamespaceContext()->isTranslationUnit())
      return true;
  }
  return false;
}

static bool isZeroSizedArray(QualType Ty) {
  if (const auto *CATy = dyn_cast<ConstantArrayType>(Ty))
    return CATy->getSize() == 0;
  return false;
}

static void checkSYCLType(Sema &S, QualType Ty, SourceRange Loc,
                          llvm::DenseSet<QualType> Visited,
                          SourceRange UsedAtLoc = SourceRange()) {
  // Not all variable types are supported inside SYCL kernels,
  // for example the quad type __float128 will cause errors in the
  // SPIR-V translation phase.
  // Here we check any potentially unsupported declaration and issue
  // a deferred diagnostic, which will be emitted iff the declaration
  // is discovered to reside in kernel code.
  // The optional UsedAtLoc param is used when the SYCL usage is at a
  // different location than the variable declaration and we need to
  // inform the user of both, e.g. struct member usage vs declaration.

  bool Emitting = false;

  //--- check types ---

  // zero length arrays
  if (isZeroSizedArray(Ty)) {
    S.SYCLDiagIfDeviceCode(Loc.getBegin(), diag::err_typecheck_zero_array_size);
    Emitting = true;
  }

  // variable length arrays
  if (Ty->isVariableArrayType()) {
    S.SYCLDiagIfDeviceCode(Loc.getBegin(), diag::err_vla_unsupported);
    Emitting = true;
  }

  // Sub-reference array or pointer, then proceed with that type.
  while (Ty->isAnyPointerType() || Ty->isArrayType())
    Ty = QualType{Ty->getPointeeOrArrayElementType(), 0};

  // __int128, __int128_t, __uint128_t, long double, __float128
  if (Ty->isSpecificBuiltinType(BuiltinType::Int128) ||
      Ty->isSpecificBuiltinType(BuiltinType::UInt128) ||
      Ty->isSpecificBuiltinType(BuiltinType::LongDouble) ||
      (Ty->isSpecificBuiltinType(BuiltinType::Float128) &&
       !S.Context.getTargetInfo().hasFloat128Type())) {
    S.SYCLDiagIfDeviceCode(Loc.getBegin(), diag::err_type_unsupported)
        << Ty.getUnqualifiedType().getCanonicalType();
    Emitting = true;
  }

  if (Emitting && UsedAtLoc.isValid())
    S.SYCLDiagIfDeviceCode(UsedAtLoc.getBegin(), diag::note_used_here);

  //--- now recurse ---
  // Pointers complicate recursion. Add this type to Visited.
  // If already there, bail out.
  if (!Visited.insert(Ty).second)
    return;

  if (const auto *ATy = dyn_cast<AttributedType>(Ty))
    return checkSYCLType(S, ATy->getModifiedType(), Loc, Visited);

  if (const auto *RD = Ty->getAsRecordDecl()) {
    for (const auto &Field : RD->fields())
      checkSYCLType(S, Field->getType(), Field->getSourceRange(), Visited, Loc);
  } else if (const auto *FPTy = dyn_cast<FunctionProtoType>(Ty)) {
    for (const auto &ParamTy : FPTy->param_types())
      checkSYCLType(S, ParamTy, Loc, Visited);
    checkSYCLType(S, FPTy->getReturnType(), Loc, Visited);
  }
}

void Sema::checkSYCLDeviceVarDecl(VarDecl *Var) {
  assert(getLangOpts().SYCLIsDevice &&
         "Should only be called during SYCL compilation");
  QualType Ty = Var->getType();
  SourceRange Loc = Var->getLocation();
  llvm::DenseSet<QualType> Visited;

  checkSYCLType(*this, Ty, Loc, Visited);
}

// Tests whether given function is a lambda function or '()' operator used as
// SYCL kernel body function (e.g. in parallel_for).
// NOTE: This is incomplete implemenation. See TODO in the FE TODO list for the
// ESIMD extension.
static bool isSYCLKernelBodyFunction(FunctionDecl *FD) {
  return FD->getOverloadedOperator() == OO_Call;
}

// Helper function to report conflicting function attributes.
// F - the function, A1 - function attribute, A2 - the attribute it conflicts
// with.
static void reportConflictingAttrs(Sema &S, FunctionDecl *F, const Attr *A1,
                                   const Attr *A2) {
  S.Diag(F->getLocation(), diag::err_conflicting_sycl_kernel_attributes);
  S.Diag(A1->getLocation(), diag::note_conflicting_attribute);
  S.Diag(A2->getLocation(), diag::note_conflicting_attribute);
  F->setInvalidDecl();
}

/// Returns the signed constant integer value represented by given expression
static int64_t getIntExprValue(const Expr *E, ASTContext &Ctx) {
  return E->getIntegerConstantExpr(Ctx)->getSExtValue();
}

class MarkDeviceFunction : public RecursiveASTVisitor<MarkDeviceFunction> {
  // Used to keep track of the constexpr depth, so we know whether to skip
  // diagnostics.
  unsigned ConstexprDepth = 0;
  struct ConstexprDepthRAII {
    MarkDeviceFunction &MDF;
    bool Increment;

    ConstexprDepthRAII(MarkDeviceFunction &MDF, bool Increment = true)
        : MDF(MDF), Increment(Increment) {
      if (Increment)
        ++MDF.ConstexprDepth;
    }
    ~ConstexprDepthRAII() {
      if (Increment)
        --MDF.ConstexprDepth;
    }
  };

public:
  MarkDeviceFunction(Sema &S)
      : RecursiveASTVisitor<MarkDeviceFunction>(), SemaRef(S) {}

  bool VisitCallExpr(CallExpr *e) {
    if (FunctionDecl *Callee = e->getDirectCallee()) {
      Callee = Callee->getCanonicalDecl();
      assert(Callee && "Device function canonical decl must be available");

      // Remember that all SYCL kernel functions have deferred
      // instantiation as template functions. It means that
      // all functions used by kernel have already been parsed and have
      // definitions.
      if (RecursiveSet.count(Callee) && !ConstexprDepth) {
        SemaRef.Diag(e->getExprLoc(), diag::warn_sycl_restrict_recursion);
        SemaRef.Diag(Callee->getSourceRange().getBegin(),
                     diag::note_sycl_recursive_function_declared_here)
            << Sema::KernelCallRecursiveFunction;
      }

      if (const CXXMethodDecl *Method = dyn_cast<CXXMethodDecl>(Callee))
        if (Method->isVirtual())
          SemaRef.Diag(e->getExprLoc(), diag::err_sycl_restrict)
              << Sema::KernelCallVirtualFunction;

      if (auto const *FD = dyn_cast<FunctionDecl>(Callee)) {
        // FIXME: We need check all target specified attributes for error if
        // that function with attribute can not be called from sycl kernel.  The
        // info is in ParsedAttr. We don't have to map from Attr to ParsedAttr
        // currently. Erich is currently working on that in LLVM, once that is
        // committed we need to change this".
        if (FD->hasAttr<DLLImportAttr>()) {
          SemaRef.Diag(e->getExprLoc(), diag::err_sycl_restrict)
              << Sema::KernelCallDllimportFunction;
          SemaRef.Diag(FD->getLocation(), diag::note_callee_decl) << FD;
        }
      }
      // Specifically check if the math library function corresponding to this
      // builtin is supported for SYCL
      unsigned BuiltinID = Callee->getBuiltinID();
      if (BuiltinID && !IsSyclMathFunc(BuiltinID)) {
        StringRef Name = SemaRef.Context.BuiltinInfo.getName(BuiltinID);
        SemaRef.Diag(e->getExprLoc(), diag::err_builtin_target_unsupported)
            << Name << "SYCL device";
      }
    } else if (!SemaRef.getLangOpts().SYCLAllowFuncPtr &&
               !e->isTypeDependent() &&
               !isa<CXXPseudoDestructorExpr>(e->getCallee()))
      SemaRef.Diag(e->getExprLoc(), diag::err_sycl_restrict)
          << Sema::KernelCallFunctionPointer;
    return true;
  }

  bool VisitCXXTypeidExpr(CXXTypeidExpr *E) {
    SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) << Sema::KernelRTTI;
    return true;
  }

  bool VisitCXXDynamicCastExpr(const CXXDynamicCastExpr *E) {
    SemaRef.Diag(E->getExprLoc(), diag::err_sycl_restrict) << Sema::KernelRTTI;
    return true;
  }

  // Skip checking rules on variables initialized during constant evaluation.
  bool TraverseVarDecl(VarDecl *VD) {
    ConstexprDepthRAII R(*this, VD->isConstexpr());
    return RecursiveASTVisitor::TraverseVarDecl(VD);
  }

  // Skip checking rules on template arguments, since these are constant
  // expressions.
  bool TraverseTemplateArgumentLoc(const TemplateArgumentLoc &ArgLoc) {
    ConstexprDepthRAII R(*this);
    return RecursiveASTVisitor::TraverseTemplateArgumentLoc(ArgLoc);
  }

  // Skip checking the static assert, both components are required to be
  // constant expressions.
  bool TraverseStaticAssertDecl(StaticAssertDecl *D) {
    ConstexprDepthRAII R(*this);
    return RecursiveASTVisitor::TraverseStaticAssertDecl(D);
  }

  // Make sure we skip the condition of the case, since that is a constant
  // expression.
  bool TraverseCaseStmt(CaseStmt *S) {
    {
      ConstexprDepthRAII R(*this);
      if (!TraverseStmt(S->getLHS()))
        return false;
      if (!TraverseStmt(S->getRHS()))
        return false;
    }
    return TraverseStmt(S->getSubStmt());
  }

  // Skip checking the size expr, since a constant array type loc's size expr is
  // a constant expression.
  bool TraverseConstantArrayTypeLoc(const ConstantArrayTypeLoc &ArrLoc) {
    if (!TraverseTypeLoc(ArrLoc.getElementLoc()))
      return false;

    ConstexprDepthRAII R(*this);
    return TraverseStmt(ArrLoc.getSizeExpr());
  }

  // The call graph for this translation unit.
  CallGraph SYCLCG;
  // The set of functions called by a kernel function.
  llvm::SmallPtrSet<FunctionDecl *, 10> KernelSet;
  // The set of recursive functions identified while building the
  // kernel set, this is used for error diagnostics.
  llvm::SmallPtrSet<FunctionDecl *, 10> RecursiveSet;
  // Determines whether the function FD is recursive.
  // CalleeNode is a function which is called either directly
  // or indirectly from FD.  If recursion is detected then create
  // diagnostic notes on each function as the callstack is unwound.
  void CollectKernelSet(FunctionDecl *CalleeNode, FunctionDecl *FD,
                        llvm::SmallPtrSet<FunctionDecl *, 10> VisitedSet) {
    // We're currently checking CalleeNode on a different
    // trace through the CallGraph, we avoid infinite recursion
    // by using KernelSet to keep track of this.
    if (!KernelSet.insert(CalleeNode).second)
      // Previously seen, stop recursion.
      return;
    if (CallGraphNode *N = SYCLCG.getNode(CalleeNode)) {
      for (const CallGraphNode *CI : *N) {
        if (FunctionDecl *Callee = dyn_cast<FunctionDecl>(CI->getDecl())) {
          Callee = Callee->getCanonicalDecl();
          if (VisitedSet.count(Callee)) {
            // There's a stack frame to visit this Callee above
            // this invocation. Do not recurse here.
            RecursiveSet.insert(Callee);
            RecursiveSet.insert(CalleeNode);
          } else {
            VisitedSet.insert(Callee);
            CollectKernelSet(Callee, FD, VisitedSet);
            VisitedSet.erase(Callee);
          }
        }
      }
    }
  }

  // Traverses over CallGraph to collect list of attributes applied to
  // functions called by SYCLKernel (either directly and indirectly) which needs
  // to be propagated down to callers and applied to SYCL kernels.
  // For example, reqd_work_group_size, vec_len_hint, reqd_sub_group_size
  // Attributes applied to SYCLKernel are also included
  // Returns the kernel body function found during traversal.
  FunctionDecl *
  CollectPossibleKernelAttributes(FunctionDecl *SYCLKernel,
                                  llvm::SmallPtrSet<Attr *, 4> &Attrs) {
    typedef std::pair<FunctionDecl *, FunctionDecl *> ChildParentPair;
    llvm::SmallPtrSet<FunctionDecl *, 16> Visited;
    llvm::SmallVector<ChildParentPair, 16> WorkList;
    WorkList.push_back({SYCLKernel, nullptr});
    FunctionDecl *KernelBody = nullptr;

    while (!WorkList.empty()) {
      FunctionDecl *FD = WorkList.back().first;
      FunctionDecl *ParentFD = WorkList.back().second;

      // To implement rounding-up of a parallel-for range the
      // SYCL header implementation modifies the kernel call like this:
      // auto Wrapper = [=](TransformedArgType Arg) {
      //  if (Arg[0] >= NumWorkItems[0])
      //    return;
      //  Arg.set_allowed_range(NumWorkItems);
      //  KernelFunc(Arg);
      // };
      //
      // This transformation leads to a condition where a kernel body
      // function becomes callable from a new kernel body function.
      // Hence this test.
      if ((ParentFD == KernelBody) && isSYCLKernelBodyFunction(FD))
        KernelBody = FD;

      if ((ParentFD == SYCLKernel) && isSYCLKernelBodyFunction(FD)) {
        assert(!KernelBody && "inconsistent call graph - only one kernel body "
                              "function can be called");
        KernelBody = FD;
      }
      WorkList.pop_back();
      if (!Visited.insert(FD).second)
        continue; // We've already seen this Decl

      if (auto *A = FD->getAttr<IntelReqdSubGroupSizeAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<ReqdWorkGroupSizeAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLIntelKernelArgsRestrictAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLIntelNumSimdWorkItemsAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLIntelSchedulerTargetFmaxMhzAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLIntelMaxWorkGroupSizeAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLIntelMaxGlobalWorkDimAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLIntelNoGlobalWorkOffsetAttr>())
        Attrs.insert(A);

      if (auto *A = FD->getAttr<SYCLSimdAttr>())
        Attrs.insert(A);

      // Allow the kernel attribute "use_stall_enable_clusters" only on lambda
      // functions and function objects that are called directly from a kernel
      // (i.e. the one passed to the single_task or parallel_for functions).
      // For all other cases, emit a warning and ignore.
      if (auto *A = FD->getAttr<SYCLIntelUseStallEnableClustersAttr>()) {
        if (ParentFD == SYCLKernel) {
          Attrs.insert(A);
        } else {
          SemaRef.Diag(A->getLocation(), diag::warn_attribute_ignored) << A;
          FD->dropAttr<SYCLIntelUseStallEnableClustersAttr>();
        }
      }

      // Propagate the explicit SIMD attribute through call graph - it is used
      // to distinguish ESIMD code in ESIMD LLVM passes.
      if (KernelBody && KernelBody->hasAttr<SYCLSimdAttr>() &&
          (KernelBody != FD) && !FD->hasAttr<SYCLSimdAttr>())
        FD->addAttr(SYCLSimdAttr::CreateImplicit(SemaRef.getASTContext()));

      // Attribute "loop_fuse" can be applied explicitly on kernel function.
      // Attribute should not be propagated from device functions to kernel.
      if (auto *A = FD->getAttr<SYCLIntelLoopFuseAttr>()) {
        if (ParentFD == SYCLKernel) {
          Attrs.insert(A);
        }
      }

      // TODO: vec_len_hint should be handled here

      CallGraphNode *N = SYCLCG.getNode(FD);
      if (!N)
        continue;

      for (const CallGraphNode *CI : *N) {
        if (auto *Callee = dyn_cast<FunctionDecl>(CI->getDecl())) {
          Callee = Callee->getMostRecentDecl();
          if (!Visited.count(Callee))
            WorkList.push_back({Callee, FD});
        }
      }
    }
    return KernelBody;
  }

private:
  Sema &SemaRef;
};

class KernelBodyTransform : public TreeTransform<KernelBodyTransform> {
public:
  KernelBodyTransform(std::pair<DeclaratorDecl *, DeclaratorDecl *> &MPair,
                      Sema &S)
      : TreeTransform<KernelBodyTransform>(S), MappingPair(MPair), SemaRef(S) {}
  bool AlwaysRebuild() { return true; }

  ExprResult TransformDeclRefExpr(DeclRefExpr *DRE) {
    auto Ref = dyn_cast<DeclaratorDecl>(DRE->getDecl());
    if (Ref && Ref == MappingPair.first) {
      auto NewDecl = MappingPair.second;
      return DeclRefExpr::Create(
          SemaRef.getASTContext(), DRE->getQualifierLoc(),
          DRE->getTemplateKeywordLoc(), NewDecl, false, DRE->getNameInfo(),
          NewDecl->getType(), DRE->getValueKind());
    }
    return DRE;
  }

private:
  std::pair<DeclaratorDecl *, DeclaratorDecl *> MappingPair;
  Sema &SemaRef;
};

// Searches for a call to PFWG lambda function and captures it.
class FindPFWGLambdaFnVisitor
    : public RecursiveASTVisitor<FindPFWGLambdaFnVisitor> {
public:
  // LambdaObjTy - lambda type of the PFWG lambda object
  FindPFWGLambdaFnVisitor(const CXXRecordDecl *LambdaObjTy)
      : LambdaFn(nullptr), LambdaObjTy(LambdaObjTy) {}

  bool VisitCallExpr(CallExpr *Call) {
    auto *M = dyn_cast<CXXMethodDecl>(Call->getDirectCallee());
    if (!M || (M->getOverloadedOperator() != OO_Call))
      return true;
    const int NumPFWGLambdaArgs = 2; // group and lambda obj
    if (Call->getNumArgs() != NumPFWGLambdaArgs)
      return true;
    if (!Util::isSyclType(Call->getArg(1)->getType(), "group", true /*Tmpl*/))
      return true;
    if (Call->getArg(0)->getType()->getAsCXXRecordDecl() != LambdaObjTy)
      return true;
    LambdaFn = M; // call to PFWG lambda found - record the lambda
    return false; // ... and stop searching
  }

  // Returns the captured lambda function or nullptr;
  CXXMethodDecl *getLambdaFn() const { return LambdaFn; }

private:
  CXXMethodDecl *LambdaFn;
  const CXXRecordDecl *LambdaObjTy;
};

class MarkWIScopeFnVisitor : public RecursiveASTVisitor<MarkWIScopeFnVisitor> {
public:
  MarkWIScopeFnVisitor(ASTContext &Ctx) : Ctx(Ctx) {}

  bool VisitCXXMemberCallExpr(CXXMemberCallExpr *Call) {
    FunctionDecl *Callee = Call->getDirectCallee();
    if (!Callee)
      // not a direct call - continue search
      return true;
    QualType Ty = Ctx.getRecordType(Call->getRecordDecl());
    if (!Util::isSyclType(Ty, "group", true /*Tmpl*/))
      // not a member of cl::sycl::group - continue search
      return true;
    auto Name = Callee->getName();
    if (((Name != "parallel_for_work_item") && (Name != "wait_for")) ||
        Callee->hasAttr<SYCLScopeAttr>())
      return true;
    // it is a call to cl::sycl::group::parallel_for_work_item/wait_for -
    // mark the callee
    Callee->addAttr(
        SYCLScopeAttr::CreateImplicit(Ctx, SYCLScopeAttr::Level::WorkItem));
    // continue search as there can be other PFWI or wait_for calls
    return true;
  }

private:
  ASTContext &Ctx;
};

static bool isSYCLPrivateMemoryVar(VarDecl *VD) {
  return Util::isSyclType(VD->getType(), "private_memory", true /*Tmpl*/);
}

static void addScopeAttrToLocalVars(CXXMethodDecl &F) {
  for (Decl *D : F.decls()) {
    VarDecl *VD = dyn_cast<VarDecl>(D);

    if (!VD || isa<ParmVarDecl>(VD) ||
        VD->getStorageDuration() != StorageDuration::SD_Automatic)
      continue;
    // Local variables of private_memory type in the WG scope still have WI
    // scope, all the rest - WG scope. Simple logic
    // "if no scope than it is WG scope" won't work, because compiler may add
    // locals not declared in user code (lambda object parameter, byval
    // arguments) which will result in alloca w/o any attribute, so need WI
    // scope too.
    SYCLScopeAttr::Level L = isSYCLPrivateMemoryVar(VD)
                                 ? SYCLScopeAttr::Level::WorkItem
                                 : SYCLScopeAttr::Level::WorkGroup;
    VD->addAttr(SYCLScopeAttr::CreateImplicit(F.getASTContext(), L));
  }
}

/// Return method by name
static CXXMethodDecl *getMethodByName(const CXXRecordDecl *CRD,
                                      StringRef MethodName) {
  CXXMethodDecl *Method;
  auto It = std::find_if(CRD->methods().begin(), CRD->methods().end(),
                         [MethodName](const CXXMethodDecl *Method) {
                           return Method->getNameAsString() == MethodName;
                         });
  Method = (It != CRD->methods().end()) ? *It : nullptr;
  return Method;
}

static KernelInvocationKind
getKernelInvocationKind(FunctionDecl *KernelCallerFunc) {
  return llvm::StringSwitch<KernelInvocationKind>(KernelCallerFunc->getName())
      .Case("kernel_single_task", InvokeSingleTask)
      .Case("kernel_parallel_for", InvokeParallelFor)
      .Case("kernel_parallel_for_work_group", InvokeParallelForWorkGroup)
      .Default(InvokeUnknown);
}

static const CXXRecordDecl *getKernelObjectType(FunctionDecl *Caller) {
  assert(Caller->getNumParams() > 0 && "Insufficient kernel parameters");

  QualType KernelParamTy = Caller->getParamDecl(0)->getType();
  // In SYCL 2020 kernels are now passed by reference.
  if (KernelParamTy->isReferenceType())
    return KernelParamTy->getPointeeCXXRecordDecl();

  // SYCL 1.2.1
  return KernelParamTy->getAsCXXRecordDecl();
}

/// Creates a kernel parameter descriptor
/// \param Src  field declaration to construct name from
/// \param Ty   the desired parameter type
/// \return     the constructed descriptor
static ParamDesc makeParamDesc(const FieldDecl *Src, QualType Ty) {
  ASTContext &Ctx = Src->getASTContext();
  std::string Name = (Twine("_arg_") + Src->getName()).str();
  return std::make_tuple(Ty, &Ctx.Idents.get(Name),
                         Ctx.getTrivialTypeSourceInfo(Ty));
}

static ParamDesc makeParamDesc(ASTContext &Ctx, const CXXBaseSpecifier &Src,
                               QualType Ty) {
  // TODO: There is no name for the base available, but duplicate names are
  // seemingly already possible, so we'll give them all the same name for now.
  // This only happens with the accessor types.
  std::string Name = "_arg__base";
  return std::make_tuple(Ty, &Ctx.Idents.get(Name),
                         Ctx.getTrivialTypeSourceInfo(Ty));
}

/// \return the target of given SYCL accessor type
static target getAccessTarget(const ClassTemplateSpecializationDecl *AccTy) {
  return static_cast<target>(
      AccTy->getTemplateArgs()[3].getAsIntegral().getExtValue());
}

// The first template argument to the kernel caller function is used to identify
// the kernel itself.
static QualType calculateKernelNameType(ASTContext &Ctx,
                                        FunctionDecl *KernelCallerFunc) {
  const TemplateArgumentList *TAL =
      KernelCallerFunc->getTemplateSpecializationArgs();
  assert(TAL && "No template argument info");
  return TAL->get(0).getAsType().getCanonicalType();
}

// Gets a name for the OpenCL kernel function, calculated from the first
// template argument of the kernel caller function.
static std::pair<std::string, std::string>
constructKernelName(Sema &S, FunctionDecl *KernelCallerFunc,
                    MangleContext &MC) {
  QualType KernelNameType =
      calculateKernelNameType(S.getASTContext(), KernelCallerFunc);

  SmallString<256> Result;
  llvm::raw_svector_ostream Out(Result);

  MC.mangleTypeName(KernelNameType, Out);

  return {std::string(Out.str()),
          PredefinedExpr::ComputeName(S.getASTContext(),
                                      PredefinedExpr::UniqueStableNameType,
                                      KernelNameType)};
}

// anonymous namespace so these don't get linkage.
namespace {

template <typename T> struct bind_param { using type = T; };

template <> struct bind_param<CXXBaseSpecifier &> {
  using type = const CXXBaseSpecifier &;
};

template <> struct bind_param<FieldDecl *&> { using type = FieldDecl *; };

template <> struct bind_param<FieldDecl *const &> { using type = FieldDecl *; };

template <typename T> using bind_param_t = typename bind_param<T>::type;

class KernelObjVisitor {
  Sema &SemaRef;

  template <typename ParentTy, typename... HandlerTys>
  void VisitUnionImpl(const CXXRecordDecl *Owner, ParentTy &Parent,
                      const CXXRecordDecl *Wrapper, HandlerTys &... Handlers) {
    (void)std::initializer_list<int>{
        (Handlers.enterUnion(Owner, Parent), 0)...};
    VisitRecordHelper(Wrapper, Wrapper->fields(), Handlers...);
    (void)std::initializer_list<int>{
        (Handlers.leaveUnion(Owner, Parent), 0)...};
  }

  // These enable handler execution only when previous Handlers succeed.
  template <typename... Tn>
  bool handleField(FieldDecl *FD, QualType FDTy, Tn &&... tn) {
    bool result = true;
    (void)std::initializer_list<int>{(result = result && tn(FD, FDTy), 0)...};
    return result;
  }
  template <typename... Tn>
  bool handleField(const CXXBaseSpecifier &BD, QualType BDTy, Tn &&... tn) {
    bool result = true;
    std::initializer_list<int>{(result = result && tn(BD, BDTy), 0)...};
    return result;
  }

// This definition using std::bind is necessary because of a gcc 7.x bug.
#define KF_FOR_EACH(FUNC, Item, Qt)                                            \
  handleField(                                                                 \
      Item, Qt,                                                                \
      std::bind(static_cast<bool (std::decay_t<decltype(Handlers)>::*)(        \
                    bind_param_t<decltype(Item)>, QualType)>(                  \
                    &std::decay_t<decltype(Handlers)>::FUNC),                  \
                std::ref(Handlers), _1, _2)...)

  // The following simpler definition works with gcc 8.x and later.
  //#define KF_FOR_EACH(FUNC) \
//  handleField(Field, FieldTy, ([&](FieldDecl *FD, QualType FDTy) { \
//                return Handlers.f(FD, FDTy); \
//              })...)

  // Parent contains the FieldDecl or CXXBaseSpecifier that was used to enter
  // the Wrapper structure that we're currently visiting. Owner is the parent
  // type (which doesn't exist in cases where it is a FieldDecl in the
  // 'root'), and Wrapper is the current struct being unwrapped.
  template <typename ParentTy, typename... HandlerTys>
  void visitComplexRecord(const CXXRecordDecl *Owner, ParentTy &Parent,
                          const CXXRecordDecl *Wrapper, QualType RecordTy,
                          HandlerTys &... Handlers) {
    (void)std::initializer_list<int>{
        (Handlers.enterStruct(Owner, Parent, RecordTy), 0)...};
    VisitRecordHelper(Wrapper, Wrapper->bases(), Handlers...);
    VisitRecordHelper(Wrapper, Wrapper->fields(), Handlers...);
    (void)std::initializer_list<int>{
        (Handlers.leaveStruct(Owner, Parent, RecordTy), 0)...};
  }

  template <typename ParentTy, typename... HandlerTys>
  void visitSimpleRecord(const CXXRecordDecl *Owner, ParentTy &Parent,
                         const CXXRecordDecl *Wrapper, QualType RecordTy,
                         HandlerTys &... Handlers) {
    (void)std::initializer_list<int>{
        (Handlers.handleNonDecompStruct(Owner, Parent, RecordTy), 0)...};
  }

  template <typename ParentTy, typename... HandlerTys>
  void visitRecord(const CXXRecordDecl *Owner, ParentTy &Parent,
                   const CXXRecordDecl *Wrapper, QualType RecordTy,
                   HandlerTys &... Handlers);

  template <typename ParentTy, typename... HandlerTys>
  void VisitUnion(const CXXRecordDecl *Owner, ParentTy &Parent,
                  const CXXRecordDecl *Wrapper, HandlerTys &... Handlers);

  template <typename... HandlerTys>
  void VisitRecordHelper(const CXXRecordDecl *Owner,
                         clang::CXXRecordDecl::base_class_const_range Range,
                         HandlerTys &... Handlers) {
    for (const auto &Base : Range) {
      QualType BaseTy = Base.getType();
      // Handle accessor class as base
      if (Util::isSyclAccessorType(BaseTy)) {
        (void)std::initializer_list<int>{
            (Handlers.handleSyclAccessorType(Owner, Base, BaseTy), 0)...};
      } else if (Util::isSyclStreamType(BaseTy)) {
        // Handle stream class as base
        (void)std::initializer_list<int>{
            (Handlers.handleSyclStreamType(Owner, Base, BaseTy), 0)...};
      } else
        // For all other bases, visit the record
        visitRecord(Owner, Base, BaseTy->getAsCXXRecordDecl(), BaseTy,
                    Handlers...);
    }
  }

  template <typename... HandlerTys>
  void VisitRecordHelper(const CXXRecordDecl *Owner,
                         RecordDecl::field_range Range,
                         HandlerTys &... Handlers) {
    VisitRecordFields(Owner, Handlers...);
  }

  // FIXME: Can this be refactored/handled some other way?
  template <typename ParentTy, typename... HandlerTys>
  void visitStreamRecord(const CXXRecordDecl *Owner, ParentTy &Parent,
                         CXXRecordDecl *Wrapper, QualType RecordTy,
                         HandlerTys &... Handlers) {
    (void)std::initializer_list<int>{
        (Handlers.enterStream(Owner, Parent, RecordTy), 0)...};
    for (const auto &Field : Wrapper->fields()) {
      QualType FieldTy = Field->getType();
      // Required to initialize accessors inside streams.
      if (Util::isSyclAccessorType(FieldTy))
        KF_FOR_EACH(handleSyclAccessorType, Field, FieldTy);
    }
    (void)std::initializer_list<int>{
        (Handlers.leaveStream(Owner, Parent, RecordTy), 0)...};
  }

  template <typename... HandlerTys>
  void visitArrayElementImpl(const CXXRecordDecl *Owner, FieldDecl *ArrayField,
                             QualType ElementTy, uint64_t Index,
                             HandlerTys &... Handlers) {
    (void)std::initializer_list<int>{
        (Handlers.nextElement(ElementTy, Index), 0)...};
    visitField(Owner, ArrayField, ElementTy, Handlers...);
  }

  template <typename... HandlerTys>
  void visitFirstArrayElement(const CXXRecordDecl *Owner, FieldDecl *ArrayField,
                              QualType ElementTy, HandlerTys &... Handlers) {
    visitArrayElementImpl(Owner, ArrayField, ElementTy, 0, Handlers...);
  }
  template <typename... HandlerTys>
  void visitNthArrayElement(const CXXRecordDecl *Owner, FieldDecl *ArrayField,
                            QualType ElementTy, uint64_t Index,
                            HandlerTys &... Handlers);

  template <typename... HandlerTys>
  void visitSimpleArray(const CXXRecordDecl *Owner, FieldDecl *Field,
                        QualType ArrayTy, HandlerTys &... Handlers) {
    (void)std::initializer_list<int>{
        (Handlers.handleSimpleArrayType(Field, ArrayTy), 0)...};
  }

  template <typename... HandlerTys>
  void visitComplexArray(const CXXRecordDecl *Owner, FieldDecl *Field,
                         QualType ArrayTy, HandlerTys &... Handlers) {
    // Array workflow is:
    // handleArrayType
    // enterArray
    // nextElement
    // VisitField (same as before, note that The FieldDecl is the of array
    // itself, not the element)
    // ... repeat per element, opt-out for duplicates.
    // leaveArray

    if (!KF_FOR_EACH(handleArrayType, Field, ArrayTy))
      return;

    const ConstantArrayType *CAT =
        SemaRef.getASTContext().getAsConstantArrayType(ArrayTy);
    assert(CAT && "Should only be called on constant-size array.");
    QualType ET = CAT->getElementType();
    uint64_t ElemCount = CAT->getSize().getZExtValue();
    assert(ElemCount > 0 && "SYCL prohibits 0 sized arrays");

    (void)std::initializer_list<int>{
        (Handlers.enterArray(Field, ArrayTy, ET), 0)...};

    visitFirstArrayElement(Owner, Field, ET, Handlers...);
    for (uint64_t Index = 1; Index < ElemCount; ++Index)
      visitNthArrayElement(Owner, Field, ET, Index, Handlers...);

    (void)std::initializer_list<int>{
        (Handlers.leaveArray(Field, ArrayTy, ET), 0)...};
  }

  template <typename... HandlerTys>
  void visitArray(const CXXRecordDecl *Owner, FieldDecl *Field,
                  QualType ArrayTy, HandlerTys &... Handlers);

  template <typename... HandlerTys>
  void visitField(const CXXRecordDecl *Owner, FieldDecl *Field,
                  QualType FieldTy, HandlerTys &... Handlers) {
    if (Util::isSyclAccessorType(FieldTy))
      KF_FOR_EACH(handleSyclAccessorType, Field, FieldTy);
    else if (Util::isSyclSamplerType(FieldTy))
      KF_FOR_EACH(handleSyclSamplerType, Field, FieldTy);
    else if (Util::isSyclHalfType(FieldTy))
      KF_FOR_EACH(handleSyclHalfType, Field, FieldTy);
    else if (Util::isSyclSpecConstantType(FieldTy))
      KF_FOR_EACH(handleSyclSpecConstantType, Field, FieldTy);
    else if (Util::isSyclStreamType(FieldTy)) {
      CXXRecordDecl *RD = FieldTy->getAsCXXRecordDecl();
      // Handle accessors in stream class.
      KF_FOR_EACH(handleSyclStreamType, Field, FieldTy);
      visitStreamRecord(Owner, Field, RD, FieldTy, Handlers...);
    } else if (FieldTy->isStructureOrClassType()) {
      if (KF_FOR_EACH(handleStructType, Field, FieldTy)) {
        CXXRecordDecl *RD = FieldTy->getAsCXXRecordDecl();
        visitRecord(Owner, Field, RD, FieldTy, Handlers...);
      }
    } else if (FieldTy->isUnionType()) {
      if (KF_FOR_EACH(handleUnionType, Field, FieldTy)) {
        CXXRecordDecl *RD = FieldTy->getAsCXXRecordDecl();
        VisitUnion(Owner, Field, RD, Handlers...);
      }
    } else if (FieldTy->isReferenceType())
      KF_FOR_EACH(handleReferenceType, Field, FieldTy);
    else if (FieldTy->isPointerType())
      KF_FOR_EACH(handlePointerType, Field, FieldTy);
    else if (FieldTy->isArrayType())
      visitArray(Owner, Field, FieldTy, Handlers...);
    else if (FieldTy->isScalarType() || FieldTy->isVectorType())
      KF_FOR_EACH(handleScalarType, Field, FieldTy);
    else
      KF_FOR_EACH(handleOtherType, Field, FieldTy);
  }

public:
  KernelObjVisitor(Sema &S) : SemaRef(S) {}

  template <typename... HandlerTys>
  void VisitRecordBases(const CXXRecordDecl *KernelFunctor,
                        HandlerTys &... Handlers) {
    VisitRecordHelper(KernelFunctor, KernelFunctor->bases(), Handlers...);
  }

  // A visitor function that dispatches to functions as defined in
  // SyclKernelFieldHandler for the purposes of kernel generation.
  template <typename... HandlerTys>
  void VisitRecordFields(const CXXRecordDecl *Owner, HandlerTys &... Handlers) {
    for (const auto Field : Owner->fields())
      visitField(Owner, Field, Field->getType(), Handlers...);
  }
#undef KF_FOR_EACH
};

// A base type that the SYCL OpenCL Kernel construction task uses to implement
// individual tasks.
class SyclKernelFieldHandlerBase {
public:
  static constexpr const bool VisitUnionBody = false;
  static constexpr const bool VisitNthArrayElement = true;
  // Opt-in based on whether we should visit inside simple containers (structs,
  // arrays). All of the 'check' types should likely be true, the int-header,
  // and kernel decl creation types should not.
  static constexpr const bool VisitInsideSimpleContainers = true;
  // Mark these virtual so that we can use override in the implementer classes,
  // despite virtual dispatch never being used.

  // Accessor can be a base class or a field decl, so both must be handled.
  virtual bool handleSyclAccessorType(const CXXRecordDecl *,
                                      const CXXBaseSpecifier &, QualType) {
    return true;
  }
  virtual bool handleSyclAccessorType(FieldDecl *, QualType) { return true; }
  virtual bool handleSyclSamplerType(const CXXRecordDecl *,
                                     const CXXBaseSpecifier &, QualType) {
    return true;
  }
  virtual bool handleSyclSamplerType(FieldDecl *, QualType) { return true; }
  virtual bool handleSyclSpecConstantType(FieldDecl *, QualType) {
    return true;
  }
  virtual bool handleSyclStreamType(const CXXRecordDecl *,
                                    const CXXBaseSpecifier &, QualType) {
    return true;
  }
  virtual bool handleSyclStreamType(FieldDecl *, QualType) { return true; }
  virtual bool handleSyclHalfType(const CXXRecordDecl *,
                                  const CXXBaseSpecifier &, QualType) {
    return true;
  }
  virtual bool handleSyclHalfType(FieldDecl *, QualType) { return true; }
  virtual bool handleStructType(FieldDecl *, QualType) { return true; }
  virtual bool handleUnionType(FieldDecl *, QualType) { return true; }
  virtual bool handleReferenceType(FieldDecl *, QualType) { return true; }
  virtual bool handlePointerType(FieldDecl *, QualType) { return true; }
  virtual bool handleArrayType(FieldDecl *, QualType) { return true; }
  virtual bool handleScalarType(FieldDecl *, QualType) { return true; }
  // Most handlers shouldn't be handling this, just the field checker.
  virtual bool handleOtherType(FieldDecl *, QualType) { return true; }

  // Handle a simple struct that doesn't need to be decomposed, only called on
  // handlers with VisitInsideSimpleContainers as false.  Replaces
  // handleStructType, enterStruct, leaveStruct, and visiting of sub-elements.
  virtual bool handleNonDecompStruct(const CXXRecordDecl *, FieldDecl *,
                                     QualType) {
    return true;
  }
  virtual bool handleNonDecompStruct(const CXXRecordDecl *,
                                     const CXXBaseSpecifier &, QualType) {
    return true;
  }

  // Instead of handleArrayType, enterArray, leaveArray, and nextElement (plus
  // descending down the elements), this function gets called in the event of an
  // array containing simple elements (even in the case of an MD array).
  virtual bool handleSimpleArrayType(FieldDecl *, QualType) { return true; }

  // The following are only used for keeping track of where we are in the base
  // class/field graph. Int Headers use this to calculate offset, most others
  // don't have a need for these.

  virtual bool enterStruct(const CXXRecordDecl *, FieldDecl *, QualType) {
    return true;
  }
  virtual bool leaveStruct(const CXXRecordDecl *, FieldDecl *, QualType) {
    return true;
  }
  virtual bool enterStream(const CXXRecordDecl *, FieldDecl *, QualType) {
    return true;
  }
  virtual bool leaveStream(const CXXRecordDecl *, FieldDecl *, QualType) {
    return true;
  }
  virtual bool enterStruct(const CXXRecordDecl *, const CXXBaseSpecifier &,
                           QualType) {
    return true;
  }
  virtual bool leaveStruct(const CXXRecordDecl *, const CXXBaseSpecifier &,
                           QualType) {
    return true;
  }
  virtual bool enterUnion(const CXXRecordDecl *, FieldDecl *) { return true; }
  virtual bool leaveUnion(const CXXRecordDecl *, FieldDecl *) { return true; }

  // The following are used for stepping through array elements.
  virtual bool enterArray(FieldDecl *, QualType ArrayTy, QualType ElementTy) {
    return true;
  }
  virtual bool leaveArray(FieldDecl *, QualType ArrayTy, QualType ElementTy) {
    return true;
  }

  virtual bool nextElement(QualType, uint64_t) { return true; }

  virtual ~SyclKernelFieldHandlerBase() = default;
};

// A class to act as the direct base for all the SYCL OpenCL Kernel construction
// tasks that contains a reference to Sema (and potentially any other
// universally required data).
class SyclKernelFieldHandler : public SyclKernelFieldHandlerBase {
protected:
  Sema &SemaRef;
  SyclKernelFieldHandler(Sema &S) : SemaRef(S) {}
};

// A class to represent the 'do nothing' case for filtering purposes.
class SyclEmptyHandler final : public SyclKernelFieldHandlerBase {};
SyclEmptyHandler GlobalEmptyHandler;

template <bool Keep, typename H> struct HandlerFilter;
template <typename H> struct HandlerFilter<true, H> {
  H &Handler;
  HandlerFilter(H &Handler) : Handler(Handler) {}
};
template <typename H> struct HandlerFilter<false, H> {
  SyclEmptyHandler &Handler = GlobalEmptyHandler;
  HandlerFilter(H &Handler) {}
};

template <bool B, bool... Rest> struct AnyTrue;

template <bool B> struct AnyTrue<B> { static constexpr bool Value = B; };

template <bool B, bool... Rest> struct AnyTrue {
  static constexpr bool Value = B || AnyTrue<Rest...>::Value;
};

template <bool B, bool... Rest> struct AllTrue;

template <bool B> struct AllTrue<B> { static constexpr bool Value = B; };

template <bool B, bool... Rest> struct AllTrue {
  static constexpr bool Value = B && AllTrue<Rest...>::Value;
};

template <typename ParentTy, typename... Handlers>
void KernelObjVisitor::VisitUnion(const CXXRecordDecl *Owner, ParentTy &Parent,
                                  const CXXRecordDecl *Wrapper,
                                  Handlers &... handlers) {
  // Don't continue descending if none of the handlers 'care'. This could be 'if
  // constexpr' starting in C++17.  Until then, we have to count on the
  // optimizer to realize "if (false)" is a dead branch.
  if (AnyTrue<Handlers::VisitUnionBody...>::Value)
    VisitUnionImpl(
        Owner, Parent, Wrapper,
        HandlerFilter<Handlers::VisitUnionBody, Handlers>(handlers).Handler...);
}

template <typename... Handlers>
void KernelObjVisitor::visitNthArrayElement(const CXXRecordDecl *Owner,
                                            FieldDecl *ArrayField,
                                            QualType ElementTy, uint64_t Index,
                                            Handlers &... handlers) {
  // Don't continue descending if none of the handlers 'care'. This could be 'if
  // constexpr' starting in C++17.  Until then, we have to count on the
  // optimizer to realize "if (false)" is a dead branch.
  if (AnyTrue<Handlers::VisitNthArrayElement...>::Value)
    visitArrayElementImpl(
        Owner, ArrayField, ElementTy, Index,
        HandlerFilter<Handlers::VisitNthArrayElement, Handlers>(handlers)
            .Handler...);
}

template <typename ParentTy, typename... HandlerTys>
void KernelObjVisitor::visitRecord(const CXXRecordDecl *Owner, ParentTy &Parent,
                                   const CXXRecordDecl *Wrapper,
                                   QualType RecordTy,
                                   HandlerTys &... Handlers) {
  RecordDecl *RD = RecordTy->getAsRecordDecl();
  assert(RD && "should not be null.");
  if (RD->hasAttr<SYCLRequiresDecompositionAttr>()) {
    // If this container requires decomposition, we have to visit it as
    // 'complex', so all handlers are called in this case with the 'complex'
    // case.
    visitComplexRecord(Owner, Parent, Wrapper, RecordTy, Handlers...);
  } else {
    // "Simple" Containers are those that do NOT need to be decomposed,
    // "Complex" containers are those that DO. In the case where the container
    // does NOT need to be decomposed, we can call VisitSimpleRecord on the
    // handlers that have opted-out of VisitInsideSimpleContainers. The 'if'
    // makes sure we only do that if at least 1 has opted out.
    if (!AllTrue<HandlerTys::VisitInsideSimpleContainers...>::Value)
      visitSimpleRecord(
          Owner, Parent, Wrapper, RecordTy,
          HandlerFilter<!HandlerTys::VisitInsideSimpleContainers, HandlerTys>(
              Handlers)
              .Handler...);

    // Even though this is a 'simple' container, some handlers (via
    // VisitInsideSimpleContainers = true) need to treat it as if it needs
    // decomposing, so we call VisitComplexRecord iif at least one has.
    if (AnyTrue<HandlerTys::VisitInsideSimpleContainers...>::Value)
      visitComplexRecord(
          Owner, Parent, Wrapper, RecordTy,
          HandlerFilter<HandlerTys::VisitInsideSimpleContainers, HandlerTys>(
              Handlers)
              .Handler...);
  }
}

template <typename... HandlerTys>
void KernelObjVisitor::visitArray(const CXXRecordDecl *Owner, FieldDecl *Field,
                                  QualType ArrayTy, HandlerTys &... Handlers) {

  if (Field->hasAttr<SYCLRequiresDecompositionAttr>()) {
    visitComplexArray(Owner, Field, ArrayTy, Handlers...);
  } else {
    if (!AllTrue<HandlerTys::VisitInsideSimpleContainers...>::Value)
      visitSimpleArray(
          Owner, Field, ArrayTy,
          HandlerFilter<!HandlerTys::VisitInsideSimpleContainers, HandlerTys>(
              Handlers)
              .Handler...);

    if (AnyTrue<HandlerTys::VisitInsideSimpleContainers...>::Value)
      visitComplexArray(
          Owner, Field, ArrayTy,
          HandlerFilter<HandlerTys::VisitInsideSimpleContainers, HandlerTys>(
              Handlers)
              .Handler...);
  }
}

// A type to check the validity of all of the argument types.
class SyclKernelFieldChecker : public SyclKernelFieldHandler {
  bool IsInvalid = false;
  DiagnosticsEngine &Diag;
  // Check whether the object should be disallowed from being copied to kernel.
  // Return true if not copyable, false if copyable.
  bool checkNotCopyableToKernel(const FieldDecl *FD, const QualType &FieldTy) {
    if (FieldTy->isArrayType()) {
      if (const auto *CAT =
              SemaRef.getASTContext().getAsConstantArrayType(FieldTy)) {
        QualType ET = CAT->getElementType();
        return checkNotCopyableToKernel(FD, ET);
      }
      return Diag.Report(FD->getLocation(),
                         diag::err_sycl_non_constant_array_type)
             << FieldTy;
    }

    if (SemaRef.getASTContext().getLangOpts().SYCLStdLayoutKernelParams)
      if (!FieldTy->isStandardLayoutType())
        return Diag.Report(FD->getLocation(),
                           diag::err_sycl_non_std_layout_type)
               << FieldTy;

    if (!FieldTy->isStructureOrClassType())
      return false;

    CXXRecordDecl *RD =
        cast<CXXRecordDecl>(FieldTy->getAs<RecordType>()->getDecl());
    if (!RD->hasTrivialCopyConstructor())
      return Diag.Report(FD->getLocation(),
                         diag::err_sycl_non_trivially_copy_ctor_dtor_type)
             << 0 << FieldTy;
    if (!RD->hasTrivialDestructor())
      return Diag.Report(FD->getLocation(),
                         diag::err_sycl_non_trivially_copy_ctor_dtor_type)
             << 1 << FieldTy;

    return false;
  }

  void checkPropertyListType(TemplateArgument PropList, SourceLocation Loc) {
    if (PropList.getKind() != TemplateArgument::ArgKind::Type) {
      SemaRef.Diag(Loc,
                   diag::err_sycl_invalid_accessor_property_template_param);
      return;
    }
    QualType PropListTy = PropList.getAsType();
    if (!Util::isAccessorPropertyListType(PropListTy)) {
      SemaRef.Diag(Loc,
                   diag::err_sycl_invalid_accessor_property_template_param);
      return;
    }
    const auto *AccPropListDecl =
        cast<ClassTemplateSpecializationDecl>(PropListTy->getAsRecordDecl());
    if (AccPropListDecl->getTemplateArgs().size() != 1) {
      SemaRef.Diag(Loc, diag::err_sycl_invalid_property_list_param_number)
          << "accessor_property_list";
      return;
    }
    const auto TemplArg = AccPropListDecl->getTemplateArgs()[0];
    if (TemplArg.getKind() != TemplateArgument::ArgKind::Pack) {
      SemaRef.Diag(Loc,
                   diag::err_sycl_invalid_accessor_property_list_template_param)
          << /*accessor_property_list*/ 0 << /*parameter pack*/ 0;
      return;
    }
    for (TemplateArgument::pack_iterator Prop = TemplArg.pack_begin();
         Prop != TemplArg.pack_end(); ++Prop) {
      if (Prop->getKind() != TemplateArgument::ArgKind::Type) {
        SemaRef.Diag(
            Loc, diag::err_sycl_invalid_accessor_property_list_template_param)
            << /*accessor_property_list pack argument*/ 1 << /*type*/ 1;
        return;
      }
      QualType PropTy = Prop->getAsType();
      if (Util::isSyclBufferLocationType(PropTy))
        checkBufferLocationType(PropTy, Loc);
    }
  }

  void checkBufferLocationType(QualType PropTy, SourceLocation Loc) {
    const auto *PropDecl =
        cast<ClassTemplateSpecializationDecl>(PropTy->getAsRecordDecl());
    if (PropDecl->getTemplateArgs().size() != 1) {
      SemaRef.Diag(Loc, diag::err_sycl_invalid_property_list_param_number)
          << "buffer_location";
      return;
    }
    const auto BufferLoc = PropDecl->getTemplateArgs()[0];
    if (BufferLoc.getKind() != TemplateArgument::ArgKind::Integral) {
      SemaRef.Diag(Loc,
                   diag::err_sycl_invalid_accessor_property_list_template_param)
          << /*buffer_location*/ 2 << /*non-negative integer*/ 2;
      return;
    }
    int LocationID = static_cast<int>(BufferLoc.getAsIntegral().getExtValue());
    if (LocationID < 0) {
      SemaRef.Diag(Loc,
                   diag::err_sycl_invalid_accessor_property_list_template_param)
          << /*buffer_location*/ 2 << /*non-negative integer*/ 2;
      return;
    }
  }

  void checkAccessorType(QualType Ty, SourceRange Loc) {
    assert(Util::isSyclAccessorType(Ty) &&
           "Should only be called on SYCL accessor types.");

    const RecordDecl *RecD = Ty->getAsRecordDecl();
    if (const ClassTemplateSpecializationDecl *CTSD =
            dyn_cast<ClassTemplateSpecializationDecl>(RecD)) {
      const TemplateArgumentList &TAL = CTSD->getTemplateArgs();
      TemplateArgument TA = TAL.get(0);
      const QualType TemplateArgTy = TA.getAsType();

      if (TAL.size() > 5)
        checkPropertyListType(TAL.get(5), Loc.getBegin());
      llvm::DenseSet<QualType> Visited;
      checkSYCLType(SemaRef, TemplateArgTy, Loc, Visited);
    }
  }

public:
  SyclKernelFieldChecker(Sema &S)
      : SyclKernelFieldHandler(S), Diag(S.getASTContext().getDiagnostics()) {}
  static constexpr const bool VisitNthArrayElement = false;
  bool isValid() { return !IsInvalid; }

  bool handleReferenceType(FieldDecl *FD, QualType FieldTy) final {
    Diag.Report(FD->getLocation(), diag::err_bad_kernel_param_type) << FieldTy;
    IsInvalid = true;
    return isValid();
  }

  bool handleStructType(FieldDecl *FD, QualType FieldTy) final {
    IsInvalid |= checkNotCopyableToKernel(FD, FieldTy);
    return isValid();
  }

  bool handleSyclAccessorType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                              QualType FieldTy) final {
    checkAccessorType(FieldTy, BS.getBeginLoc());
    return isValid();
  }

  bool handleSyclAccessorType(FieldDecl *FD, QualType FieldTy) final {
    checkAccessorType(FieldTy, FD->getLocation());
    return isValid();
  }

  bool handleArrayType(FieldDecl *FD, QualType FieldTy) final {
    IsInvalid |= checkNotCopyableToKernel(FD, FieldTy);
    return isValid();
  }

  bool handlePointerType(FieldDecl *FD, QualType FieldTy) final {
    while (FieldTy->isAnyPointerType()) {
      FieldTy = QualType{FieldTy->getPointeeOrArrayElementType(), 0};
      if (FieldTy->isVariableArrayType()) {
        Diag.Report(FD->getLocation(), diag::err_vla_unsupported);
        IsInvalid = true;
        break;
      }
    }
    return isValid();
  }

  bool handleOtherType(FieldDecl *FD, QualType FieldTy) final {
    Diag.Report(FD->getLocation(), diag::err_bad_kernel_param_type) << FieldTy;
    IsInvalid = true;
    return isValid();
  }
};

// A type to check the validity of accessing accessor/sampler/stream
// types as kernel parameters inside union.
class SyclKernelUnionChecker : public SyclKernelFieldHandler {
  int UnionCount = 0;
  bool IsInvalid = false;
  DiagnosticsEngine &Diag;

public:
  SyclKernelUnionChecker(Sema &S)
      : SyclKernelFieldHandler(S), Diag(S.getASTContext().getDiagnostics()) {}
  bool isValid() { return !IsInvalid; }
  static constexpr const bool VisitUnionBody = true;
  static constexpr const bool VisitNthArrayElement = false;

  bool checkType(SourceLocation Loc, QualType Ty) {
    if (UnionCount) {
      IsInvalid = true;
      Diag.Report(Loc, diag::err_bad_union_kernel_param_members) << Ty;
    }
    return isValid();
  }

  bool enterUnion(const CXXRecordDecl *RD, FieldDecl *FD) {
    ++UnionCount;
    return true;
  }

  bool leaveUnion(const CXXRecordDecl *RD, FieldDecl *FD) {
    --UnionCount;
    return true;
  }

  bool handleSyclAccessorType(FieldDecl *FD, QualType FieldTy) final {
    return checkType(FD->getLocation(), FieldTy);
  }

  bool handleSyclAccessorType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                              QualType FieldTy) final {
    return checkType(BS.getBeginLoc(), FieldTy);
  }

  bool handleSyclSamplerType(FieldDecl *FD, QualType FieldTy) final {
    return checkType(FD->getLocation(), FieldTy);
  }

  bool handleSyclSamplerType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                             QualType FieldTy) final {
    return checkType(BS.getBeginLoc(), FieldTy);
  }

  bool handleSyclStreamType(FieldDecl *FD, QualType FieldTy) final {
    return checkType(FD->getLocation(), FieldTy);
  }

  bool handleSyclStreamType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                            QualType FieldTy) final {
    return checkType(BS.getBeginLoc(), FieldTy);
  }
};

// A type to mark whether a collection requires decomposition.
class SyclKernelDecompMarker : public SyclKernelFieldHandler {
  llvm::SmallVector<bool, 16> CollectionStack;

public:
  static constexpr const bool VisitUnionBody = false;
  static constexpr const bool VisitNthArrayElement = false;

  SyclKernelDecompMarker(Sema &S) : SyclKernelFieldHandler(S) {
    // In order to prevent checking this over and over, just add a dummy-base
    // entry.
    CollectionStack.push_back(true);
  }

  bool handleSyclAccessorType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                              QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclAccessorType(FieldDecl *, QualType) final {
    CollectionStack.back() = true;
    return true;
  }

  bool handleSyclSamplerType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                             QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclSamplerType(FieldDecl *, QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclSpecConstantType(FieldDecl *, QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclStreamType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                            QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclStreamType(FieldDecl *, QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclHalfType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                          QualType) final {
    CollectionStack.back() = true;
    return true;
  }
  bool handleSyclHalfType(FieldDecl *, QualType) final {
    CollectionStack.back() = true;
    return true;
  }

  bool handlePointerType(FieldDecl *, QualType) final {
    CollectionStack.back() = true;
    return true;
  }

  // Stream is always decomposed (and whether it gets decomposed is handled in
  // handleSyclStreamType), but we need a CollectionStack entry to capture the
  // accessors that get handled.
  bool enterStream(const CXXRecordDecl *, FieldDecl *, QualType) final {
    CollectionStack.push_back(false);
    return true;
  }
  bool leaveStream(const CXXRecordDecl *, FieldDecl *, QualType Ty) final {
    CollectionStack.pop_back();
    return true;
  }

  bool enterStruct(const CXXRecordDecl *, FieldDecl *, QualType) final {
    CollectionStack.push_back(false);
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *, FieldDecl *, QualType Ty) final {
    if (CollectionStack.pop_back_val()) {
      RecordDecl *RD = Ty->getAsRecordDecl();
      assert(RD && "should not be null.");
      if (!RD->hasAttr<SYCLRequiresDecompositionAttr>())
        RD->addAttr(SYCLRequiresDecompositionAttr::CreateImplicit(
            SemaRef.getASTContext()));
      CollectionStack.back() = true;
    }
    return true;
  }

  bool enterStruct(const CXXRecordDecl *, const CXXBaseSpecifier &,
                   QualType) final {
    CollectionStack.push_back(false);
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *, const CXXBaseSpecifier &,
                   QualType Ty) final {
    if (CollectionStack.pop_back_val()) {
      RecordDecl *RD = Ty->getAsRecordDecl();
      assert(RD && "should not be null.");
      if (!RD->hasAttr<SYCLRequiresDecompositionAttr>())
        RD->addAttr(SYCLRequiresDecompositionAttr::CreateImplicit(
            SemaRef.getASTContext()));
      CollectionStack.back() = true;
    }

    return true;
  }

  bool enterArray(FieldDecl *, QualType ArrayTy, QualType ElementTy) final {
    CollectionStack.push_back(false);
    return true;
  }

  bool leaveArray(FieldDecl *FD, QualType ArrayTy, QualType ElementTy) final {
    if (CollectionStack.pop_back_val()) {
      // Cannot assert, since in MD arrays we'll end up marking them multiple
      // times.
      if (!FD->hasAttr<SYCLRequiresDecompositionAttr>())
        FD->addAttr(SYCLRequiresDecompositionAttr::CreateImplicit(
            SemaRef.getASTContext()));
      CollectionStack.back() = true;
    }
    return true;
  }
};

// A type to Create and own the FunctionDecl for the kernel.
class SyclKernelDeclCreator : public SyclKernelFieldHandler {
  FunctionDecl *KernelDecl;
  llvm::SmallVector<ParmVarDecl *, 8> Params;
  Sema::ContextRAII FuncContext;
  // Holds the last handled field's first parameter. This doesn't store an
  // iterator as push_back invalidates iterators.
  size_t LastParamIndex = 0;
  // Keeps track of whether we are currently handling fields inside a struct.
  int StructDepth = 0;

  void addParam(const FieldDecl *FD, QualType FieldTy) {
    ParamDesc newParamDesc = makeParamDesc(FD, FieldTy);
    addParam(newParamDesc, FieldTy);
  }

  void addParam(const CXXBaseSpecifier &BS, QualType FieldTy) {
    ParamDesc newParamDesc =
        makeParamDesc(SemaRef.getASTContext(), BS, FieldTy);
    addParam(newParamDesc, FieldTy);
  }

  void addParam(ParamDesc newParamDesc, QualType FieldTy) {
    // Create a new ParmVarDecl based on the new info.
    ASTContext &Ctx = SemaRef.getASTContext();
    auto *NewParam = ParmVarDecl::Create(
        Ctx, KernelDecl, SourceLocation(), SourceLocation(),
        std::get<1>(newParamDesc), std::get<0>(newParamDesc),
        std::get<2>(newParamDesc), SC_None, /*DefArg*/ nullptr);
    NewParam->setScopeInfo(0, Params.size());
    NewParam->setIsUsed();

    LastParamIndex = Params.size();
    Params.push_back(NewParam);
  }

  // Handle accessor properties. If any properties were found in
  // the accessor_property_list - add the appropriate attributes to ParmVarDecl.
  void handleAccessorPropertyList(ParmVarDecl *Param,
                                  const CXXRecordDecl *RecordDecl,
                                  SourceLocation Loc) {
    const auto *AccTy = cast<ClassTemplateSpecializationDecl>(RecordDecl);
    if (AccTy->getTemplateArgs().size() < 6)
      return;
    const auto PropList = cast<TemplateArgument>(AccTy->getTemplateArgs()[5]);
    QualType PropListTy = PropList.getAsType();
    const auto *AccPropListDecl =
        cast<ClassTemplateSpecializationDecl>(PropListTy->getAsRecordDecl());
    const auto TemplArg = AccPropListDecl->getTemplateArgs()[0];
    // Move through TemplateArgs list of a property list and search for
    // properties. If found - apply the appropriate attribute to ParmVarDecl.
    for (TemplateArgument::pack_iterator Prop = TemplArg.pack_begin();
         Prop != TemplArg.pack_end(); ++Prop) {
      QualType PropTy = Prop->getAsType();
      if (Util::isSyclBufferLocationType(PropTy))
        handleBufferLocationProperty(Param, PropTy, Loc);
    }
  }

  // Obtain an integer value stored in a template parameter of buffer_location
  // property to pass it to buffer_location kernel attribute
  void handleBufferLocationProperty(ParmVarDecl *Param, QualType PropTy,
                                    SourceLocation Loc) {
    // If we have more than 1 buffer_location properties on a single
    // accessor - emit an error
    if (Param->hasAttr<SYCLIntelBufferLocationAttr>()) {
      SemaRef.Diag(Loc, diag::err_sycl_compiletime_property_duplication)
          << "buffer_location";
      return;
    }
    ASTContext &Ctx = SemaRef.getASTContext();
    const auto *PropDecl =
        cast<ClassTemplateSpecializationDecl>(PropTy->getAsRecordDecl());
    const auto BufferLoc = PropDecl->getTemplateArgs()[0];
    int LocationID = static_cast<int>(BufferLoc.getAsIntegral().getExtValue());
    Param->addAttr(
        SYCLIntelBufferLocationAttr::CreateImplicit(Ctx, LocationID));
  }

  // All special SYCL objects must have __init method. We extract types for
  // kernel parameters from __init method parameters. We will use __init method
  // and kernel parameters which we build here to initialize special objects in
  // the kernel body.
  bool handleSpecialType(FieldDecl *FD, QualType FieldTy,
                         bool isAccessorType = false) {
    const auto *RecordDecl = FieldTy->getAsCXXRecordDecl();
    assert(RecordDecl && "The accessor/sampler must be a RecordDecl");
    const std::string &MethodName =
        KernelDecl->hasAttr<SYCLSimdAttr>() && isAccessorType
            ? InitESIMDMethodName
            : InitMethodName;
    CXXMethodDecl *InitMethod = getMethodByName(RecordDecl, MethodName);
    assert(InitMethod && "The accessor/sampler must have the __init method");

    // Don't do -1 here because we count on this to be the first parameter added
    // (if any).
    size_t ParamIndex = Params.size();
    for (const ParmVarDecl *Param : InitMethod->parameters()) {
      QualType ParamTy = Param->getType();
      addParam(FD, ParamTy.getCanonicalType());
      if (ParamTy.getTypePtr()->isPointerType() && isAccessorType) {
        handleAccessorPropertyList(Params.back(), RecordDecl,
                                   FD->getLocation());
        if (KernelDecl->hasAttr<SYCLSimdAttr>())
          // In ESIMD kernels accessor's pointer argument needs to be marked
          Params.back()->addAttr(
              SYCLSimdAccessorPtrAttr::CreateImplicit(SemaRef.getASTContext()));
      }
    }
    LastParamIndex = ParamIndex;
    return true;
  }

  static void setKernelImplicitAttrs(ASTContext &Context, FunctionDecl *FD,
                                     StringRef Name, bool IsSIMDKernel) {
    // Set implicit attributes.
    FD->addAttr(OpenCLKernelAttr::CreateImplicit(Context));
    FD->addAttr(AsmLabelAttr::CreateImplicit(Context, Name));
    FD->addAttr(ArtificialAttr::CreateImplicit(Context));
    if (IsSIMDKernel)
      FD->addAttr(SYCLSimdAttr::CreateImplicit(Context));
  }

  static FunctionDecl *createKernelDecl(ASTContext &Ctx, StringRef Name,
                                        SourceLocation Loc, bool IsInline,
                                        bool IsSIMDKernel) {
    // Create this with no prototype, and we can fix this up after we've seen
    // all the params.
    FunctionProtoType::ExtProtoInfo Info(CC_OpenCLKernel);
    QualType FuncType = Ctx.getFunctionType(Ctx.VoidTy, {}, Info);

    FunctionDecl *FD = FunctionDecl::Create(
        Ctx, Ctx.getTranslationUnitDecl(), Loc, Loc, &Ctx.Idents.get(Name),
        FuncType, Ctx.getTrivialTypeSourceInfo(Ctx.VoidTy), SC_None);
    FD->setImplicitlyInline(IsInline);
    setKernelImplicitAttrs(Ctx, FD, Name, IsSIMDKernel);

    // Add kernel to translation unit to see it in AST-dump.
    Ctx.getTranslationUnitDecl()->addDecl(FD);
    return FD;
  }

public:
  static constexpr const bool VisitInsideSimpleContainers = false;
  SyclKernelDeclCreator(Sema &S, StringRef Name, SourceLocation Loc,
                        bool IsInline, bool IsSIMDKernel)
      : SyclKernelFieldHandler(S),
        KernelDecl(createKernelDecl(S.getASTContext(), Name, Loc, IsInline,
                                    IsSIMDKernel)),
        FuncContext(SemaRef, KernelDecl) {}

  ~SyclKernelDeclCreator() {
    ASTContext &Ctx = SemaRef.getASTContext();
    FunctionProtoType::ExtProtoInfo Info(CC_OpenCLKernel);

    SmallVector<QualType, 8> ArgTys;
    std::transform(std::begin(Params), std::end(Params),
                   std::back_inserter(ArgTys),
                   [](const ParmVarDecl *PVD) { return PVD->getType(); });

    QualType FuncType = Ctx.getFunctionType(Ctx.VoidTy, ArgTys, Info);
    KernelDecl->setType(FuncType);
    KernelDecl->setParams(Params);

    SemaRef.addSyclDeviceDecl(KernelDecl);
  }

  bool enterStream(const CXXRecordDecl *RD, FieldDecl *FD, QualType Ty) final {
    return enterStruct(RD, FD, Ty);
  }

  bool leaveStream(const CXXRecordDecl *RD, FieldDecl *FD, QualType Ty) final {
    return leaveStruct(RD, FD, Ty);
  }

  bool enterStruct(const CXXRecordDecl *, FieldDecl *, QualType) final {
    ++StructDepth;
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *, FieldDecl *, QualType) final {
    --StructDepth;
    return true;
  }

  bool enterStruct(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                   QualType FieldTy) final {
    ++StructDepth;
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                   QualType FieldTy) final {
    --StructDepth;
    return true;
  }

  bool handleSyclAccessorType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                              QualType FieldTy) final {
    const auto *RecordDecl = FieldTy->getAsCXXRecordDecl();
    assert(RecordDecl && "The accessor/sampler must be a RecordDecl");
    const std::string MethodName = KernelDecl->hasAttr<SYCLSimdAttr>()
                                       ? InitESIMDMethodName
                                       : InitMethodName;
    CXXMethodDecl *InitMethod = getMethodByName(RecordDecl, MethodName);
    assert(InitMethod && "The accessor/sampler must have the __init method");

    // Don't do -1 here because we count on this to be the first parameter added
    // (if any).
    size_t ParamIndex = Params.size();
    for (const ParmVarDecl *Param : InitMethod->parameters()) {
      QualType ParamTy = Param->getType();
      addParam(BS, ParamTy.getCanonicalType());
      if (ParamTy.getTypePtr()->isPointerType())
        handleAccessorPropertyList(Params.back(), RecordDecl, BS.getBeginLoc());
    }
    LastParamIndex = ParamIndex;
    return true;
  }

  bool handleSyclAccessorType(FieldDecl *FD, QualType FieldTy) final {
    return handleSpecialType(FD, FieldTy, /*isAccessorType*/ true);
  }

  bool handleSyclSamplerType(FieldDecl *FD, QualType FieldTy) final {
    return handleSpecialType(FD, FieldTy);
  }

  RecordDecl *wrapField(FieldDecl *Field, QualType FieldTy) {
    RecordDecl *WrapperClass =
        SemaRef.getASTContext().buildImplicitRecord("__wrapper_class");
    WrapperClass->startDefinition();
    Field = FieldDecl::Create(
        SemaRef.getASTContext(), WrapperClass, SourceLocation(),
        SourceLocation(), /*Id=*/nullptr, FieldTy,
        SemaRef.getASTContext().getTrivialTypeSourceInfo(FieldTy,
                                                         SourceLocation()),
        /*BW=*/nullptr, /*Mutable=*/false, /*InitStyle=*/ICIS_NoInit);
    Field->setAccess(AS_public);
    WrapperClass->addDecl(Field);
    WrapperClass->completeDefinition();
    return WrapperClass;
  };

  bool handlePointerType(FieldDecl *FD, QualType FieldTy) final {
    // USM allows to use raw pointers instead of buffers/accessors, but these
    // pointers point to the specially allocated memory. For pointer fields we
    // add a kernel argument with the same type as field but global address
    // space, because OpenCL requires it.
    QualType PointeeTy = FieldTy->getPointeeType();
    Qualifiers Quals = PointeeTy.getQualifiers();
    auto AS = Quals.getAddressSpace();
    // Leave global_device and global_host address spaces as is to help FPGA
    // device in memory allocations
    if (AS != LangAS::opencl_global_device && AS != LangAS::opencl_global_host)
      Quals.setAddressSpace(LangAS::opencl_global);
    PointeeTy = SemaRef.getASTContext().getQualifiedType(
        PointeeTy.getUnqualifiedType(), Quals);
    QualType ModTy = SemaRef.getASTContext().getPointerType(PointeeTy);
    // When the kernel is generated, struct type kernel arguments are
    // decomposed; i.e. the parameters of the kernel are the fields of the
    // struct, and not the struct itself. This causes an error in the backend
    // when the struct field is a pointer, since non-USM pointers cannot be
    // passed directly. To work around this issue, all pointers inside the
    // struct are wrapped in a generated '__wrapper_class'.
    if (StructDepth) {
      RecordDecl *WrappedPointer = wrapField(FD, ModTy);
      ModTy = SemaRef.getASTContext().getRecordType(WrappedPointer);
    }

    addParam(FD, ModTy);
    return true;
  }

  bool handleSimpleArrayType(FieldDecl *FD, QualType FieldTy) final {
    // Arrays are always wrapped in a struct since they cannot be passed
    // directly.
    RecordDecl *WrappedArray = wrapField(FD, FieldTy);
    QualType ModTy = SemaRef.getASTContext().getRecordType(WrappedArray);
    addParam(FD, ModTy);
    return true;
  }

  bool handleScalarType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *, FieldDecl *FD,
                             QualType Ty) final {
    addParam(FD, Ty);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *Base,
                             const CXXBaseSpecifier &BS, QualType Ty) final {
    addParam(BS, Ty);
    return true;
  }

  bool handleUnionType(FieldDecl *FD, QualType FieldTy) final {
    return handleScalarType(FD, FieldTy);
  }

  bool handleSyclHalfType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy);
    return true;
  }

  bool handleSyclStreamType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy);
    return true;
  }

  bool handleSyclStreamType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                            QualType FieldTy) final {
    // FIXME SYCL stream should be usable as a base type
    // See https://github.com/intel/llvm/issues/1552
    return true;
  }

  void setBody(CompoundStmt *KB) { KernelDecl->setBody(KB); }

  FunctionDecl *getKernelDecl() { return KernelDecl; }

  llvm::ArrayRef<ParmVarDecl *> getParamVarDeclsForCurrentField() {
    return ArrayRef<ParmVarDecl *>(std::begin(Params) + LastParamIndex,
                                   std::end(Params));
  }
  using SyclKernelFieldHandler::handleSyclHalfType;
  using SyclKernelFieldHandler::handleSyclSamplerType;
};

class SyclKernelArgsSizeChecker : public SyclKernelFieldHandler {
  SourceLocation KernelLoc;
  unsigned SizeOfParams = 0;
  bool IsSIMD = false;

  void addParam(QualType ArgTy) {
    SizeOfParams +=
        SemaRef.getASTContext().getTypeSizeInChars(ArgTy).getQuantity();
  }

  bool handleSpecialType(QualType FieldTy) {
    const CXXRecordDecl *RecordDecl = FieldTy->getAsCXXRecordDecl();
    assert(RecordDecl && "The accessor/sampler must be a RecordDecl");
    const std::string &MethodName =
        IsSIMD ? InitESIMDMethodName : InitMethodName;
    CXXMethodDecl *InitMethod = getMethodByName(RecordDecl, MethodName);
    assert(InitMethod && "The accessor/sampler must have the __init method");
    for (const ParmVarDecl *Param : InitMethod->parameters())
      addParam(Param->getType());
    return true;
  }

public:
  static constexpr const bool VisitInsideSimpleContainers = false;
  SyclKernelArgsSizeChecker(Sema &S, SourceLocation Loc, bool IsSIMD)
      : SyclKernelFieldHandler(S), KernelLoc(Loc), IsSIMD(IsSIMD) {}

  ~SyclKernelArgsSizeChecker() {
    if (SizeOfParams > MaxKernelArgsSize)
      SemaRef.Diag(KernelLoc, diag::warn_sycl_kernel_too_big_args)
          << SizeOfParams << MaxKernelArgsSize;
  }

  bool handleSyclAccessorType(FieldDecl *FD, QualType FieldTy) final {
    return handleSpecialType(FieldTy);
  }

  bool handleSyclAccessorType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                              QualType FieldTy) final {
    return handleSpecialType(FieldTy);
  }

  bool handleSyclSamplerType(FieldDecl *FD, QualType FieldTy) final {
    return handleSpecialType(FieldTy);
  }

  bool handleSyclSamplerType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                             QualType FieldTy) final {
    return handleSpecialType(FieldTy);
  }

  bool handlePointerType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FieldTy);
    return true;
  }

  bool handleScalarType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FieldTy);
    return true;
  }

  bool handleSimpleArrayType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FieldTy);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *, FieldDecl *FD,
                             QualType Ty) final {
    addParam(Ty);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *Base,
                             const CXXBaseSpecifier &BS, QualType Ty) final {
    addParam(Ty);
    return true;
  }

  bool handleUnionType(FieldDecl *FD, QualType FieldTy) final {
    return handleScalarType(FD, FieldTy);
  }

  bool handleSyclHalfType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FieldTy);
    return true;
  }

  bool handleSyclStreamType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FieldTy);
    return true;
  }
  bool handleSyclStreamType(const CXXRecordDecl *, const CXXBaseSpecifier &,
                            QualType FieldTy) final {
    addParam(FieldTy);
    return true;
  }
  using SyclKernelFieldHandler::handleSyclHalfType;
};

static const CXXMethodDecl *getOperatorParens(const CXXRecordDecl *Rec) {
  for (const auto *MD : Rec->methods()) {
    if (MD->getOverloadedOperator() == OO_Call)
      return MD;
  }
  return nullptr;
}

static bool isESIMDKernelType(const CXXRecordDecl *KernelObjType) {
  const CXXMethodDecl *OpParens = getOperatorParens(KernelObjType);
  return (OpParens != nullptr) && OpParens->hasAttr<SYCLSimdAttr>();
}

class SyclKernelBodyCreator : public SyclKernelFieldHandler {
  SyclKernelDeclCreator &DeclCreator;
  llvm::SmallVector<Stmt *, 16> BodyStmts;
  llvm::SmallVector<InitListExpr *, 16> CollectionInitExprs;
  llvm::SmallVector<Stmt *, 16> FinalizeStmts;
  // This collection contains the information required to add/remove information
  // about arrays as we enter them.  The InitializedEntity component is
  // necessary for initializing child members.  uin64_t is the index of the
  // current element being worked on, which is updated every time we visit
  // nextElement.
  llvm::SmallVector<std::pair<InitializedEntity, uint64_t>, 8> ArrayInfos;
  VarDecl *KernelObjClone;
  InitializedEntity VarEntity;
  const CXXRecordDecl *KernelObj;
  llvm::SmallVector<Expr *, 16> MemberExprBases;
  FunctionDecl *KernelCallerFunc;
  SourceLocation KernelCallerSrcLoc; // KernelCallerFunc source location.
  // Contains a count of how many containers we're in.  This is used by the
  // pointer-struct-wrapping code to ensure that we don't try to wrap
  // non-top-level pointers.
  uint64_t StructDepth = 0;

  // Using the statements/init expressions that we've created, this generates
  // the kernel body compound stmt. CompoundStmt needs to know its number of
  // statements in advance to allocate it, so we cannot do this as we go along.
  CompoundStmt *createKernelBody() {
    assert(CollectionInitExprs.size() == 1 &&
           "Should have been popped down to just the first one");
    KernelObjClone->setInit(CollectionInitExprs.back());
    Stmt *FunctionBody = KernelCallerFunc->getBody();

    ParmVarDecl *KernelObjParam = *(KernelCallerFunc->param_begin());

    // DeclRefExpr with valid source location but with decl which is not marked
    // as used is invalid.
    KernelObjClone->setIsUsed();
    std::pair<DeclaratorDecl *, DeclaratorDecl *> MappingPair =
        std::make_pair(KernelObjParam, KernelObjClone);

    // Push the Kernel function scope to ensure the scope isn't empty
    SemaRef.PushFunctionScope();
    KernelBodyTransform KBT(MappingPair, SemaRef);
    Stmt *NewBody = KBT.TransformStmt(FunctionBody).get();
    BodyStmts.push_back(NewBody);

    BodyStmts.insert(BodyStmts.end(), FinalizeStmts.begin(),
                     FinalizeStmts.end());
    return CompoundStmt::Create(SemaRef.getASTContext(), BodyStmts, {}, {});
  }

  void markParallelWorkItemCalls() {
    if (getKernelInvocationKind(KernelCallerFunc) ==
        InvokeParallelForWorkGroup) {
      FindPFWGLambdaFnVisitor V(KernelObj);
      V.TraverseStmt(KernelCallerFunc->getBody());
      CXXMethodDecl *WGLambdaFn = V.getLambdaFn();
      assert(WGLambdaFn && "PFWG lambda not found");
      // Mark the function that it "works" in a work group scope:
      // NOTE: In case of parallel_for_work_item the marker call itself is
      // marked with work item scope attribute, here  the '()' operator of the
      // object passed as parameter is marked. This is an optimization -
      // there are a lot of locals created at parallel_for_work_group
      // scope before calling the lambda - it is more efficient to have
      // all of them in the private address space rather then sharing via
      // the local AS. See parallel_for_work_group implementation in the
      // SYCL headers.
      if (!WGLambdaFn->hasAttr<SYCLScopeAttr>()) {
        WGLambdaFn->addAttr(SYCLScopeAttr::CreateImplicit(
            SemaRef.getASTContext(), SYCLScopeAttr::Level::WorkGroup));
        // Search and mark parallel_for_work_item calls:
        MarkWIScopeFnVisitor MarkWIScope(SemaRef.getASTContext());
        MarkWIScope.TraverseDecl(WGLambdaFn);
        // Now mark local variables declared in the PFWG lambda with work group
        // scope attribute
        addScopeAttrToLocalVars(*WGLambdaFn);
      }
    }
  }

  // Creates a DeclRefExpr to the ParmVar that represents the current field.
  Expr *createParamReferenceExpr() {
    ParmVarDecl *KernelParameter =
        DeclCreator.getParamVarDeclsForCurrentField()[0];

    QualType ParamType = KernelParameter->getOriginalType();
    Expr *DRE = SemaRef.BuildDeclRefExpr(KernelParameter, ParamType, VK_LValue,
                                         KernelCallerSrcLoc);
    return DRE;
  }

  // Creates a DeclRefExpr to the ParmVar that represents the current pointer
  // field.
  Expr *createPointerParamReferenceExpr(QualType PointerTy, bool Wrapped) {
    ParmVarDecl *KernelParameter =
        DeclCreator.getParamVarDeclsForCurrentField()[0];

    QualType ParamType = KernelParameter->getOriginalType();
    Expr *DRE = SemaRef.BuildDeclRefExpr(KernelParameter, ParamType, VK_LValue,
                                         KernelCallerSrcLoc);

    // Struct Type kernel arguments are decomposed. The pointer fields are
    // then wrapped inside a compiler generated struct. Therefore when
    // generating the initializers, we have to 'unwrap' the pointer.
    if (Wrapped) {
      CXXRecordDecl *WrapperStruct = ParamType->getAsCXXRecordDecl();
      // Pointer field wrapped inside __wrapper_class
      FieldDecl *Pointer = *(WrapperStruct->field_begin());
      DRE = buildMemberExpr(DRE, Pointer);
      ParamType = Pointer->getType();
    }

    DRE = ImplicitCastExpr::Create(SemaRef.Context, ParamType,
                                   CK_LValueToRValue, DRE, /*BasePath=*/nullptr,
                                   VK_RValue, FPOptionsOverride());

    if (PointerTy->getPointeeType().getAddressSpace() !=
        ParamType->getPointeeType().getAddressSpace())
      DRE = ImplicitCastExpr::Create(SemaRef.Context, PointerTy,
                                     CK_AddressSpaceConversion, DRE, nullptr,
                                     VK_RValue, FPOptionsOverride());

    return DRE;
  }

  Expr *createSimpleArrayParamReferenceExpr(QualType ArrayTy) {
    ParmVarDecl *KernelParameter =
        DeclCreator.getParamVarDeclsForCurrentField()[0];
    QualType ParamType = KernelParameter->getOriginalType();
    Expr *DRE = SemaRef.BuildDeclRefExpr(KernelParameter, ParamType, VK_LValue,
                                         KernelCallerSrcLoc);

    // Unwrap the array.
    CXXRecordDecl *WrapperStruct = ParamType->getAsCXXRecordDecl();
    FieldDecl *ArrayField = *(WrapperStruct->field_begin());
    return buildMemberExpr(DRE, ArrayField);
  }

  // Returns 'true' if the thing we're visiting (Based on the FD/QualType pair)
  // is an element of an array.  This will determine whether we do
  // MemberExprBases in some cases or not, AND determines how we initialize
  // values.
  bool isArrayElement(const FieldDecl *FD, QualType Ty) const {
    return !SemaRef.getASTContext().hasSameType(FD->getType(), Ty);
  }

  // Creates an initialized entity for a field/item. In the case where this is a
  // field, returns a normal member initializer, if we're in a sub-array of a MD
  // array, returns an element initializer.
  InitializedEntity getFieldEntity(FieldDecl *FD, QualType Ty) {
    if (isArrayElement(FD, Ty))
      return InitializedEntity::InitializeElement(SemaRef.getASTContext(),
                                                  ArrayInfos.back().second,
                                                  ArrayInfos.back().first);
    return InitializedEntity::InitializeMember(FD, &VarEntity);
  }

  void addFieldInit(FieldDecl *FD, QualType Ty, MultiExprArg ParamRef) {
    InitializationKind InitKind =
        InitializationKind::CreateCopy(KernelCallerSrcLoc, KernelCallerSrcLoc);
    addFieldInit(FD, Ty, ParamRef, InitKind);
  }

  void addFieldInit(FieldDecl *FD, QualType Ty, MultiExprArg ParamRef,
                    InitializationKind InitKind) {
    addFieldInit(FD, Ty, ParamRef, InitKind, getFieldEntity(FD, Ty));
  }

  void addFieldInit(FieldDecl *FD, QualType Ty, MultiExprArg ParamRef,
                    InitializationKind InitKind, InitializedEntity Entity) {
    InitializationSequence InitSeq(SemaRef, Entity, InitKind, ParamRef);
    ExprResult Init = InitSeq.Perform(SemaRef, Entity, InitKind, ParamRef);

    InitListExpr *ParentILE = CollectionInitExprs.back();
    ParentILE->updateInit(SemaRef.getASTContext(), ParentILE->getNumInits(),
                          Init.get());
  }

  void addBaseInit(const CXXBaseSpecifier &BS, QualType Ty,
                   InitializationKind InitKind) {
    InitializedEntity Entity = InitializedEntity::InitializeBase(
        SemaRef.Context, &BS, /*IsInheritedVirtualBase*/ false, &VarEntity);
    InitializationSequence InitSeq(SemaRef, Entity, InitKind, None);
    ExprResult Init = InitSeq.Perform(SemaRef, Entity, InitKind, None);

    InitListExpr *ParentILE = CollectionInitExprs.back();
    ParentILE->updateInit(SemaRef.getASTContext(), ParentILE->getNumInits(),
                          Init.get());
  }

  void addSimpleBaseInit(const CXXBaseSpecifier &BS, QualType Ty) {
    InitializationKind InitKind =
        InitializationKind::CreateCopy(KernelCallerSrcLoc, KernelCallerSrcLoc);

    InitializedEntity Entity = InitializedEntity::InitializeBase(
        SemaRef.Context, &BS, /*IsInheritedVirtualBase*/ false, &VarEntity);

    Expr *ParamRef = createParamReferenceExpr();
    InitializationSequence InitSeq(SemaRef, Entity, InitKind, ParamRef);
    ExprResult Init = InitSeq.Perform(SemaRef, Entity, InitKind, ParamRef);

    InitListExpr *ParentILE = CollectionInitExprs.back();
    ParentILE->updateInit(SemaRef.getASTContext(), ParentILE->getNumInits(),
                          Init.get());
  }

  // Adds an initializer that handles a simple initialization of a field.
  void addSimpleFieldInit(FieldDecl *FD, QualType Ty) {
    Expr *ParamRef = createParamReferenceExpr();
    addFieldInit(FD, Ty, ParamRef);
  }

  MemberExpr *buildMemberExpr(Expr *Base, ValueDecl *Member) {
    DeclAccessPair MemberDAP = DeclAccessPair::make(Member, AS_none);
    MemberExpr *Result = SemaRef.BuildMemberExpr(
        Base, /*IsArrow */ false, KernelCallerSrcLoc, NestedNameSpecifierLoc(),
        KernelCallerSrcLoc, Member, MemberDAP,
        /*HadMultipleCandidates*/ false,
        DeclarationNameInfo(Member->getDeclName(), KernelCallerSrcLoc),
        Member->getType(), VK_LValue, OK_Ordinary);
    return Result;
  }

  void addFieldMemberExpr(FieldDecl *FD, QualType Ty) {
    if (!isArrayElement(FD, Ty))
      MemberExprBases.push_back(buildMemberExpr(MemberExprBases.back(), FD));
  }

  void removeFieldMemberExpr(const FieldDecl *FD, QualType Ty) {
    if (!isArrayElement(FD, Ty))
      MemberExprBases.pop_back();
  }

  void createSpecialMethodCall(const CXXRecordDecl *RD, StringRef MethodName,
                               SmallVectorImpl<Stmt *> &AddTo) {
    CXXMethodDecl *Method = getMethodByName(RD, MethodName);
    if (!Method)
      return;

    unsigned NumParams = Method->getNumParams();
    llvm::SmallVector<Expr *, 4> ParamDREs(NumParams);
    llvm::ArrayRef<ParmVarDecl *> KernelParameters =
        DeclCreator.getParamVarDeclsForCurrentField();
    for (size_t I = 0; I < NumParams; ++I) {
      QualType ParamType = KernelParameters[I]->getOriginalType();
      ParamDREs[I] = SemaRef.BuildDeclRefExpr(KernelParameters[I], ParamType,
                                              VK_LValue, KernelCallerSrcLoc);
    }

    MemberExpr *MethodME = buildMemberExpr(MemberExprBases.back(), Method);

    QualType ResultTy = Method->getReturnType();
    ExprValueKind VK = Expr::getValueKindForType(ResultTy);
    ResultTy = ResultTy.getNonLValueExprType(SemaRef.Context);
    llvm::SmallVector<Expr *, 4> ParamStmts;
    const auto *Proto = cast<FunctionProtoType>(Method->getType());
    SemaRef.GatherArgumentsForCall(KernelCallerSrcLoc, Method, Proto, 0,
                                   ParamDREs, ParamStmts);
    // [kernel_obj or wrapper object].accessor.__init(_ValueType*,
    // range<int>, range<int>, id<int>)
    AddTo.push_back(CXXMemberCallExpr::Create(
        SemaRef.Context, MethodME, ParamStmts, ResultTy, VK, KernelCallerSrcLoc,
        FPOptionsOverride()));
  }

  // Creates an empty InitListExpr of the correct number of child-inits
  // of this to append into.
  void addCollectionInitListExpr(const CXXRecordDecl *RD) {
    const ASTRecordLayout &Info =
        SemaRef.getASTContext().getASTRecordLayout(RD);
    uint64_t NumInitExprs = Info.getFieldCount() + RD->getNumBases();
    addCollectionInitListExpr(QualType(RD->getTypeForDecl(), 0), NumInitExprs);
  }

  InitListExpr *createInitListExpr(const CXXRecordDecl *RD) {
    const ASTRecordLayout &Info =
        SemaRef.getASTContext().getASTRecordLayout(RD);
    uint64_t NumInitExprs = Info.getFieldCount() + RD->getNumBases();
    return createInitListExpr(QualType(RD->getTypeForDecl(), 0), NumInitExprs);
  }

  InitListExpr *createInitListExpr(QualType InitTy, uint64_t NumChildInits) {
    InitListExpr *ILE = new (SemaRef.getASTContext()) InitListExpr(
        SemaRef.getASTContext(), KernelCallerSrcLoc, {}, KernelCallerSrcLoc);
    ILE->reserveInits(SemaRef.getASTContext(), NumChildInits);
    ILE->setType(InitTy);

    return ILE;
  }

  // Create an empty InitListExpr of the type/size for the rest of the visitor
  // to append into.
  void addCollectionInitListExpr(QualType InitTy, uint64_t NumChildInits) {

    InitListExpr *ILE = createInitListExpr(InitTy, NumChildInits);
    InitListExpr *ParentILE = CollectionInitExprs.back();
    ParentILE->updateInit(SemaRef.getASTContext(), ParentILE->getNumInits(),
                          ILE);

    CollectionInitExprs.push_back(ILE);
  }

  // FIXME Avoid creation of kernel obj clone.
  // See https://github.com/intel/llvm/issues/1544 for details.
  static VarDecl *createKernelObjClone(ASTContext &Ctx, DeclContext *DC,
                                       const CXXRecordDecl *KernelObj) {
    TypeSourceInfo *TSInfo =
        KernelObj->isLambda() ? KernelObj->getLambdaTypeInfo() : nullptr;
    VarDecl *VD = VarDecl::Create(
        Ctx, DC, KernelObj->getLocation(), KernelObj->getLocation(),
        KernelObj->getIdentifier(), QualType(KernelObj->getTypeForDecl(), 0),
        TSInfo, SC_None);

    return VD;
  }

  const std::string &getInitMethodName() const {
    bool IsSIMDKernel = isESIMDKernelType(KernelObj);
    return IsSIMDKernel ? InitESIMDMethodName : InitMethodName;
  }

  // Default inits the type, then calls the init-method in the body.
  bool handleSpecialType(FieldDecl *FD, QualType Ty) {
    addFieldInit(FD, Ty, None,
                 InitializationKind::CreateDefault(KernelCallerSrcLoc));

    addFieldMemberExpr(FD, Ty);

    const auto *RecordDecl = Ty->getAsCXXRecordDecl();
    createSpecialMethodCall(RecordDecl, getInitMethodName(), BodyStmts);

    removeFieldMemberExpr(FD, Ty);

    return true;
  }

  bool handleSpecialType(const CXXBaseSpecifier &BS, QualType Ty) {
    const auto *RecordDecl = Ty->getAsCXXRecordDecl();
    addBaseInit(BS, Ty, InitializationKind::CreateDefault(KernelCallerSrcLoc));
    createSpecialMethodCall(RecordDecl, getInitMethodName(), BodyStmts);
    return true;
  }

public:
  static constexpr const bool VisitInsideSimpleContainers = false;
  SyclKernelBodyCreator(Sema &S, SyclKernelDeclCreator &DC,
                        const CXXRecordDecl *KernelObj,
                        FunctionDecl *KernelCallerFunc)
      : SyclKernelFieldHandler(S), DeclCreator(DC),
        KernelObjClone(createKernelObjClone(S.getASTContext(),
                                            DC.getKernelDecl(), KernelObj)),
        VarEntity(InitializedEntity::InitializeVariable(KernelObjClone)),
        KernelObj(KernelObj), KernelCallerFunc(KernelCallerFunc),
        KernelCallerSrcLoc(KernelCallerFunc->getLocation()) {
    CollectionInitExprs.push_back(createInitListExpr(KernelObj));
    markParallelWorkItemCalls();

    Stmt *DS = new (S.Context) DeclStmt(DeclGroupRef(KernelObjClone),
                                        KernelCallerSrcLoc, KernelCallerSrcLoc);
    BodyStmts.push_back(DS);
    DeclRefExpr *KernelObjCloneRef = DeclRefExpr::Create(
        S.Context, NestedNameSpecifierLoc(), KernelCallerSrcLoc, KernelObjClone,
        false, DeclarationNameInfo(), QualType(KernelObj->getTypeForDecl(), 0),
        VK_LValue);
    MemberExprBases.push_back(KernelObjCloneRef);
  }

  ~SyclKernelBodyCreator() {
    CompoundStmt *KernelBody = createKernelBody();
    DeclCreator.setBody(KernelBody);
  }

  bool handleSyclAccessorType(FieldDecl *FD, QualType Ty) final {
    return handleSpecialType(FD, Ty);
  }

  bool handleSyclAccessorType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                              QualType Ty) final {
    return handleSpecialType(BS, Ty);
  }

  bool handleSyclSamplerType(FieldDecl *FD, QualType Ty) final {
    return handleSpecialType(FD, Ty);
  }

  bool handleSyclSpecConstantType(FieldDecl *FD, QualType Ty) final {
    return handleSpecialType(FD, Ty);
  }

  bool handleSyclStreamType(FieldDecl *FD, QualType Ty) final {
    // Streams just get copied as a new init.
    addSimpleFieldInit(FD, Ty);
    return true;
  }

  bool handleSyclStreamType(const CXXRecordDecl *, const CXXBaseSpecifier &BS,
                            QualType Ty) final {
    // FIXME SYCL stream should be usable as a base type
    // See https://github.com/intel/llvm/issues/1552
    return true;
  }

  bool handleSyclHalfType(FieldDecl *FD, QualType Ty) final {
    addSimpleFieldInit(FD, Ty);
    return true;
  }

  bool handlePointerType(FieldDecl *FD, QualType FieldTy) final {
    Expr *PointerRef =
        createPointerParamReferenceExpr(FieldTy, StructDepth != 0);
    addFieldInit(FD, FieldTy, PointerRef);
    return true;
  }

  bool handleSimpleArrayType(FieldDecl *FD, QualType FieldTy) final {
    Expr *ArrayRef = createSimpleArrayParamReferenceExpr(FieldTy);
    InitializationKind InitKind = InitializationKind::CreateDirect({}, {}, {});

    InitializedEntity Entity =
        InitializedEntity::InitializeMember(FD, &VarEntity, /*Implicit*/ true);

    addFieldInit(FD, FieldTy, ArrayRef, InitKind, Entity);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *, FieldDecl *FD,
                             QualType Ty) final {
    addSimpleFieldInit(FD, Ty);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *Base,
                             const CXXBaseSpecifier &BS, QualType Ty) final {
    addSimpleBaseInit(BS, Ty);
    return true;
  }

  bool handleScalarType(FieldDecl *FD, QualType FieldTy) final {
    addSimpleFieldInit(FD, FieldTy);
    return true;
  }

  bool handleUnionType(FieldDecl *FD, QualType FieldTy) final {
    addSimpleFieldInit(FD, FieldTy);
    return true;
  }

  bool enterStream(const CXXRecordDecl *RD, FieldDecl *FD, QualType Ty) final {
    ++StructDepth;
    // Add a dummy init expression to catch the accessor initializers.
    const auto *StreamDecl = Ty->getAsCXXRecordDecl();
    CollectionInitExprs.push_back(createInitListExpr(StreamDecl));

    addFieldMemberExpr(FD, Ty);
    return true;
  }

  bool leaveStream(const CXXRecordDecl *RD, FieldDecl *FD, QualType Ty) final {
    --StructDepth;
    // Stream requires that its 'init' calls happen after its accessors init
    // calls, so add them here instead.
    const auto *StreamDecl = Ty->getAsCXXRecordDecl();

    createSpecialMethodCall(StreamDecl, getInitMethodName(), BodyStmts);
    createSpecialMethodCall(StreamDecl, FinalizeMethodName, FinalizeStmts);

    removeFieldMemberExpr(FD, Ty);

    CollectionInitExprs.pop_back();
    return true;
  }

  bool enterStruct(const CXXRecordDecl *RD, FieldDecl *FD, QualType Ty) final {
    ++StructDepth;
    addCollectionInitListExpr(Ty->getAsCXXRecordDecl());

    addFieldMemberExpr(FD, Ty);
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *, FieldDecl *FD, QualType Ty) final {
    --StructDepth;
    CollectionInitExprs.pop_back();

    removeFieldMemberExpr(FD, Ty);
    return true;
  }

  bool enterStruct(const CXXRecordDecl *RD, const CXXBaseSpecifier &BS,
                   QualType) final {
    ++StructDepth;

    CXXCastPath BasePath;
    QualType DerivedTy(RD->getTypeForDecl(), 0);
    QualType BaseTy = BS.getType();
    SemaRef.CheckDerivedToBaseConversion(DerivedTy, BaseTy, KernelCallerSrcLoc,
                                         SourceRange(), &BasePath,
                                         /*IgnoreBaseAccess*/ true);
    auto Cast = ImplicitCastExpr::Create(
        SemaRef.Context, BaseTy, CK_DerivedToBase, MemberExprBases.back(),
        /* CXXCastPath=*/&BasePath, VK_LValue, FPOptionsOverride());
    MemberExprBases.push_back(Cast);

    addCollectionInitListExpr(BaseTy->getAsCXXRecordDecl());
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *RD, const CXXBaseSpecifier &BS,
                   QualType) final {
    --StructDepth;
    MemberExprBases.pop_back();
    CollectionInitExprs.pop_back();
    return true;
  }

  bool enterArray(FieldDecl *FD, QualType ArrayType,
                  QualType ElementType) final {
    uint64_t ArraySize = SemaRef.getASTContext()
                             .getAsConstantArrayType(ArrayType)
                             ->getSize()
                             .getZExtValue();
    addCollectionInitListExpr(ArrayType, ArraySize);
    ArrayInfos.emplace_back(getFieldEntity(FD, ArrayType), 0);

    // If this is the top-level array, we need to make a MemberExpr in addition
    // to an array subscript.
    addFieldMemberExpr(FD, ArrayType);
    return true;
  }

  bool nextElement(QualType, uint64_t Index) final {
    ArrayInfos.back().second = Index;

    // Pop off the last member expr base.
    if (Index != 0)
      MemberExprBases.pop_back();

    QualType SizeT = SemaRef.getASTContext().getSizeType();

    llvm::APInt IndexVal{
        static_cast<unsigned>(SemaRef.getASTContext().getTypeSize(SizeT)),
        Index, SizeT->isSignedIntegerType()};

    auto IndexLiteral = IntegerLiteral::Create(
        SemaRef.getASTContext(), IndexVal, SizeT, KernelCallerSrcLoc);

    ExprResult IndexExpr = SemaRef.CreateBuiltinArraySubscriptExpr(
        MemberExprBases.back(), KernelCallerSrcLoc, IndexLiteral,
        KernelCallerSrcLoc);

    assert(!IndexExpr.isInvalid());
    MemberExprBases.push_back(IndexExpr.get());
    return true;
  }

  bool leaveArray(FieldDecl *FD, QualType ArrayType,
                  QualType ElementType) final {
    CollectionInitExprs.pop_back();
    ArrayInfos.pop_back();

    assert(
        !SemaRef.getASTContext().getAsConstantArrayType(ArrayType)->getSize() ==
            0 &&
        "Constant arrays must have at least 1 element");
    // Remove the IndexExpr.
    MemberExprBases.pop_back();

    // Remove the field access expr as well.
    removeFieldMemberExpr(FD, ArrayType);
    return true;
  }

  using SyclKernelFieldHandler::handleSyclHalfType;
  using SyclKernelFieldHandler::handleSyclSamplerType;
};

class SyclKernelIntHeaderCreator : public SyclKernelFieldHandler {
  SYCLIntegrationHeader &Header;
  int64_t CurOffset = 0;
  llvm::SmallVector<size_t, 16> ArrayBaseOffsets;
  int StructDepth = 0;

  // A series of functions to calculate the change in offset based on the type.
  int64_t offsetOf(const FieldDecl *FD, QualType ArgTy) const {
    return isArrayElement(FD, ArgTy)
               ? 0
               : SemaRef.getASTContext().getFieldOffset(FD) / 8;
  }

  int64_t offsetOf(const CXXRecordDecl *RD, const CXXRecordDecl *Base) const {
    const ASTRecordLayout &Layout =
        SemaRef.getASTContext().getASTRecordLayout(RD);
    return Layout.getBaseClassOffset(Base).getQuantity();
  }

  void addParam(const FieldDecl *FD, QualType ArgTy,
                SYCLIntegrationHeader::kernel_param_kind_t Kind) {
    addParam(ArgTy, Kind, offsetOf(FD, ArgTy));
  }
  void addParam(QualType ArgTy, SYCLIntegrationHeader::kernel_param_kind_t Kind,
                uint64_t OffsetAdj) {
    uint64_t Size;
    Size = SemaRef.getASTContext().getTypeSizeInChars(ArgTy).getQuantity();
    Header.addParamDesc(Kind, static_cast<unsigned>(Size),
                        static_cast<unsigned>(CurOffset + OffsetAdj));
  }

  // Returns 'true' if the thing we're visiting (Based on the FD/QualType pair)
  // is an element of an array.  This will determine whether we do
  // MemberExprBases in some cases or not, AND determines how we initialize
  // values.
  bool isArrayElement(const FieldDecl *FD, QualType Ty) const {
    return !SemaRef.getASTContext().hasSameType(FD->getType(), Ty);
  }

  // Sets a flag if the kernel is a parallel_for that calls the
  // free function API "this_item".
  void setThisItemIsCalled(const CXXRecordDecl *KernelObj,
                           FunctionDecl *KernelFunc) {
    if (getKernelInvocationKind(KernelFunc) != InvokeParallelFor)
      return;

    const CXXMethodDecl *WGLambdaFn = getOperatorParens(KernelObj);
    if (!WGLambdaFn)
      return;

    // The call graph for this translation unit.
    CallGraph SYCLCG;
    SYCLCG.addToCallGraph(SemaRef.getASTContext().getTranslationUnitDecl());
    using ChildParentPair =
        std::pair<const FunctionDecl *, const FunctionDecl *>;
    llvm::SmallPtrSet<const FunctionDecl *, 16> Visited;
    llvm::SmallVector<ChildParentPair, 16> WorkList;
    WorkList.push_back({WGLambdaFn, nullptr});

    while (!WorkList.empty()) {
      const FunctionDecl *FD = WorkList.back().first;
      WorkList.pop_back();
      if (!Visited.insert(FD).second)
        continue; // We've already seen this Decl

      // Check whether this call is to sycl::this_item().
      if (Util::isSyclFunction(FD, "this_item")) {
        Header.setCallsThisItem(true);
        return;
      }

      CallGraphNode *N = SYCLCG.getNode(FD);
      if (!N)
        continue;

      for (const CallGraphNode *CI : *N) {
        if (auto *Callee = dyn_cast<FunctionDecl>(CI->getDecl())) {
          Callee = Callee->getMostRecentDecl();
          if (!Visited.count(Callee))
            WorkList.push_back({Callee, FD});
        }
      }
    }
  }

public:
  static constexpr const bool VisitInsideSimpleContainers = false;
  SyclKernelIntHeaderCreator(Sema &S, SYCLIntegrationHeader &H,
                             const CXXRecordDecl *KernelObj, QualType NameType,
                             StringRef Name, StringRef StableName,
                             FunctionDecl *KernelFunc)
      : SyclKernelFieldHandler(S), Header(H) {
    bool IsSIMDKernel = isESIMDKernelType(KernelObj);
    Header.startKernel(Name, NameType, StableName, KernelObj->getLocation(),
                       IsSIMDKernel);
    setThisItemIsCalled(KernelObj, KernelFunc);
  }

  bool handleSyclAccessorType(const CXXRecordDecl *RD,
                              const CXXBaseSpecifier &BC,
                              QualType FieldTy) final {
    const auto *AccTy =
        cast<ClassTemplateSpecializationDecl>(FieldTy->getAsRecordDecl());
    assert(AccTy->getTemplateArgs().size() >= 2 &&
           "Incorrect template args for Accessor Type");
    int Dims = static_cast<int>(
        AccTy->getTemplateArgs()[1].getAsIntegral().getExtValue());
    int Info = getAccessTarget(AccTy) | (Dims << 11);
    Header.addParamDesc(SYCLIntegrationHeader::kind_accessor, Info,
                        CurOffset +
                            offsetOf(RD, BC.getType()->getAsCXXRecordDecl()));
    return true;
  }

  bool handleSyclAccessorType(FieldDecl *FD, QualType FieldTy) final {
    const auto *AccTy =
        cast<ClassTemplateSpecializationDecl>(FieldTy->getAsRecordDecl());
    assert(AccTy->getTemplateArgs().size() >= 2 &&
           "Incorrect template args for Accessor Type");
    int Dims = static_cast<int>(
        AccTy->getTemplateArgs()[1].getAsIntegral().getExtValue());
    int Info = getAccessTarget(AccTy) | (Dims << 11);

    Header.addParamDesc(SYCLIntegrationHeader::kind_accessor, Info,
                        CurOffset + offsetOf(FD, FieldTy));
    return true;
  }

  bool handleSyclSamplerType(FieldDecl *FD, QualType FieldTy) final {
    const auto *SamplerTy = FieldTy->getAsCXXRecordDecl();
    assert(SamplerTy && "Sampler type must be a C++ record type");
    CXXMethodDecl *InitMethod = getMethodByName(SamplerTy, InitMethodName);
    assert(InitMethod && "sampler must have __init method");

    // sampler __init method has only one argument
    const ParmVarDecl *SamplerArg = InitMethod->getParamDecl(0);
    assert(SamplerArg && "sampler __init method must have sampler parameter");

    // For samplers, we do some special work to ONLY initialize the first item
    // to the InitMethod as a performance improvement presumably, so the normal
    // offsetOf calculation wouldn't work correctly. Therefore, we need to call
    // a version of addParam where we calculate the offset based on the true
    // FieldDecl/FieldType pair, rather than the SampleArg type.
    addParam(SamplerArg->getType(), SYCLIntegrationHeader::kind_sampler,
             offsetOf(FD, FieldTy));
    return true;
  }

  bool handleSyclSpecConstantType(FieldDecl *FD, QualType FieldTy) final {
    const TemplateArgumentList &TemplateArgs =
        cast<ClassTemplateSpecializationDecl>(FieldTy->getAsRecordDecl())
            ->getTemplateInstantiationArgs();
    assert(TemplateArgs.size() == 2 &&
           "Incorrect template args for spec constant type");
    // Get specialization constant ID type, which is the second template
    // argument.
    QualType SpecConstIDTy = TemplateArgs.get(1).getAsType().getCanonicalType();
    const std::string SpecConstName = PredefinedExpr::ComputeName(
        SemaRef.getASTContext(), PredefinedExpr::UniqueStableNameType,
        SpecConstIDTy);
    Header.addSpecConstant(SpecConstName, SpecConstIDTy);
    return true;
  }

  bool handlePointerType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy,
             ((StructDepth) ? SYCLIntegrationHeader::kind_std_layout
                            : SYCLIntegrationHeader::kind_pointer));
    return true;
  }

  bool handleScalarType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy, SYCLIntegrationHeader::kind_std_layout);
    return true;
  }

  bool handleSimpleArrayType(FieldDecl *FD, QualType FieldTy) final {
    // Arrays are always wrapped inside of structs, so just treat it as a simple
    // struct.
    addParam(FD, FieldTy, SYCLIntegrationHeader::kind_std_layout);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *, FieldDecl *FD,
                             QualType Ty) final {
    addParam(FD, Ty, SYCLIntegrationHeader::kind_std_layout);
    return true;
  }

  bool handleNonDecompStruct(const CXXRecordDecl *Base,
                             const CXXBaseSpecifier &, QualType Ty) final {
    addParam(Ty, SYCLIntegrationHeader::kind_std_layout,
             offsetOf(Base, Ty->getAsCXXRecordDecl()));
    return true;
  }

  bool handleUnionType(FieldDecl *FD, QualType FieldTy) final {
    return handleScalarType(FD, FieldTy);
  }

  bool handleSyclStreamType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy, SYCLIntegrationHeader::kind_std_layout);
    return true;
  }

  bool handleSyclStreamType(const CXXRecordDecl *, const CXXBaseSpecifier &BC,
                            QualType FieldTy) final {
    // FIXME SYCL stream should be usable as a base type
    // See https://github.com/intel/llvm/issues/1552
    return true;
  }

  bool handleSyclHalfType(FieldDecl *FD, QualType FieldTy) final {
    addParam(FD, FieldTy, SYCLIntegrationHeader::kind_std_layout);
    return true;
  }

  bool enterStream(const CXXRecordDecl *, FieldDecl *FD, QualType Ty) final {
    ++StructDepth;
    CurOffset += offsetOf(FD, Ty);
    return true;
  }

  bool leaveStream(const CXXRecordDecl *, FieldDecl *FD, QualType Ty) final {
    --StructDepth;
    CurOffset -= offsetOf(FD, Ty);
    return true;
  }

  bool enterStruct(const CXXRecordDecl *, FieldDecl *FD, QualType Ty) final {
    ++StructDepth;
    CurOffset += offsetOf(FD, Ty);
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *, FieldDecl *FD, QualType Ty) final {
    --StructDepth;
    CurOffset -= offsetOf(FD, Ty);
    return true;
  }

  bool enterStruct(const CXXRecordDecl *RD, const CXXBaseSpecifier &BS,
                   QualType) final {
    CurOffset += offsetOf(RD, BS.getType()->getAsCXXRecordDecl());
    return true;
  }

  bool leaveStruct(const CXXRecordDecl *RD, const CXXBaseSpecifier &BS,
                   QualType) final {
    CurOffset -= offsetOf(RD, BS.getType()->getAsCXXRecordDecl());
    return true;
  }

  bool enterArray(FieldDecl *FD, QualType ArrayTy, QualType) final {
    ArrayBaseOffsets.push_back(CurOffset + offsetOf(FD, ArrayTy));
    return true;
  }

  bool nextElement(QualType ET, uint64_t Index) final {
    int64_t Size = SemaRef.getASTContext().getTypeSizeInChars(ET).getQuantity();
    CurOffset = ArrayBaseOffsets.back() + Size * Index;
    return true;
  }

  bool leaveArray(FieldDecl *FD, QualType ArrayTy, QualType) final {
    CurOffset = ArrayBaseOffsets.pop_back_val();
    CurOffset -= offsetOf(FD, ArrayTy);
    return true;
  }

  using SyclKernelFieldHandler::enterStruct;
  using SyclKernelFieldHandler::handleSyclHalfType;
  using SyclKernelFieldHandler::handleSyclSamplerType;
  using SyclKernelFieldHandler::leaveStruct;
};

} // namespace

class SYCLKernelNameTypeVisitor
    : public TypeVisitor<SYCLKernelNameTypeVisitor>,
      public ConstTemplateArgumentVisitor<SYCLKernelNameTypeVisitor> {
  Sema &S;
  SourceLocation KernelInvocationFuncLoc;
  QualType KernelNameType;
  using InnerTypeVisitor = TypeVisitor<SYCLKernelNameTypeVisitor>;
  using InnerTemplArgVisitor =
      ConstTemplateArgumentVisitor<SYCLKernelNameTypeVisitor>;
  bool IsInvalid = false;

  void VisitTemplateArgs(ArrayRef<TemplateArgument> Args) {
    for (auto &A : Args)
      Visit(A);
  }

public:
  SYCLKernelNameTypeVisitor(Sema &S, SourceLocation KernelInvocationFuncLoc,
                            QualType KernelNameType)
      : S(S), KernelInvocationFuncLoc(KernelInvocationFuncLoc),
        KernelNameType(KernelNameType) {}

  bool isValid() { return !IsInvalid; }

  void Visit(QualType T) {
    if (T.isNull())
      return;
    const CXXRecordDecl *RD = T->getAsCXXRecordDecl();
    if (!RD) {
      if (T->isNullPtrType()) {
        S.Diag(KernelInvocationFuncLoc, diag::err_sycl_kernel_incorrectly_named)
            << KernelNameType;
        S.Diag(KernelInvocationFuncLoc, diag::note_invalid_type_in_sycl_kernel)
            << /* kernel name cannot be a type in the std namespace */ 2 << T;
        IsInvalid = true;
      }
      return;
    }
    // If KernelNameType has template args visit each template arg via
    // ConstTemplateArgumentVisitor
    if (const auto *TSD = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {
      ArrayRef<TemplateArgument> Args = TSD->getTemplateArgs().asArray();
      VisitTemplateArgs(Args);
    } else {
      InnerTypeVisitor::Visit(T.getTypePtr());
    }
  }

  void Visit(const TemplateArgument &TA) {
    if (TA.isNull())
      return;
    InnerTemplArgVisitor::Visit(TA);
  }

  void VisitEnumType(const EnumType *T) {
    const EnumDecl *ED = T->getDecl();
    if (!ED->isScoped() && !ED->isFixed()) {
      S.Diag(KernelInvocationFuncLoc, diag::err_sycl_kernel_incorrectly_named)
          << KernelNameType;
      S.Diag(KernelInvocationFuncLoc, diag::note_invalid_type_in_sycl_kernel)
          << /* Unscoped enum requires fixed underlying type */ 1
          << QualType(ED->getTypeForDecl(), 0);
      IsInvalid = true;
    }
  }

  void VisitRecordType(const RecordType *T) {
    return VisitTagDecl(T->getDecl());
  }

  void VisitTagDecl(const TagDecl *Tag) {
    bool UnnamedLambdaEnabled =
        S.getASTContext().getLangOpts().SYCLUnnamedLambda;
    const DeclContext *DeclCtx = Tag->getDeclContext();
    if (DeclCtx && !UnnamedLambdaEnabled) {
      auto *NameSpace = dyn_cast_or_null<NamespaceDecl>(DeclCtx);
      if (NameSpace && NameSpace->isStdNamespace()) {
        S.Diag(KernelInvocationFuncLoc, diag::err_sycl_kernel_incorrectly_named)
            << KernelNameType;
        S.Diag(KernelInvocationFuncLoc, diag::note_invalid_type_in_sycl_kernel)
            << /* kernel name cannot be a type in the std namespace */ 2
            << QualType(Tag->getTypeForDecl(), 0);
        IsInvalid = true;
        return;
      }
      if (!DeclCtx->isTranslationUnit() && !isa<NamespaceDecl>(DeclCtx)) {
        const bool KernelNameIsMissing = Tag->getName().empty();
        if (KernelNameIsMissing) {
          S.Diag(KernelInvocationFuncLoc,
                 diag::err_sycl_kernel_incorrectly_named)
              << KernelNameType;
          S.Diag(KernelInvocationFuncLoc,
                 diag::note_invalid_type_in_sycl_kernel)
              << /* unnamed type used in a SYCL kernel name */ 3;
          IsInvalid = true;
          return;
        }
        if (Tag->isCompleteDefinition()) {
          S.Diag(KernelInvocationFuncLoc,
                 diag::err_sycl_kernel_incorrectly_named)
              << KernelNameType;
          S.Diag(KernelInvocationFuncLoc,
                 diag::note_invalid_type_in_sycl_kernel)
              << /* kernel name is not globally-visible */ 0
              << QualType(Tag->getTypeForDecl(), 0);
          IsInvalid = true;
        } else {
          S.Diag(KernelInvocationFuncLoc, diag::warn_sycl_implicit_decl);
          S.Diag(Tag->getSourceRange().getBegin(), diag::note_previous_decl)
              << Tag->getName();
        }
      }
    }
  }

  void VisitTypeTemplateArgument(const TemplateArgument &TA) {
    QualType T = TA.getAsType();
    if (const auto *ET = T->getAs<EnumType>())
      VisitEnumType(ET);
    else
      Visit(T);
  }

  void VisitIntegralTemplateArgument(const TemplateArgument &TA) {
    QualType T = TA.getIntegralType();
    if (const EnumType *ET = T->getAs<EnumType>())
      VisitEnumType(ET);
  }

  void VisitTemplateTemplateArgument(const TemplateArgument &TA) {
    TemplateDecl *TD = TA.getAsTemplate().getAsTemplateDecl();
    assert(TD && "template declaration must be available");
    TemplateParameterList *TemplateParams = TD->getTemplateParameters();
    for (NamedDecl *P : *TemplateParams) {
      if (NonTypeTemplateParmDecl *TemplateParam =
              dyn_cast<NonTypeTemplateParmDecl>(P))
        if (const EnumType *ET = TemplateParam->getType()->getAs<EnumType>())
          VisitEnumType(ET);
    }
  }

  void VisitPackTemplateArgument(const TemplateArgument &TA) {
    VisitTemplateArgs(TA.getPackAsArray());
  }
};

void Sema::CheckSYCLKernelCall(FunctionDecl *KernelFunc, SourceRange CallLoc,
                               ArrayRef<const Expr *> Args) {
  const CXXRecordDecl *KernelObj = getKernelObjectType(KernelFunc);
  QualType KernelNameType =
      calculateKernelNameType(getASTContext(), KernelFunc);
  if (!KernelObj) {
    Diag(Args[0]->getExprLoc(), diag::err_sycl_kernel_not_function_object);
    KernelFunc->setInvalidDecl();
    return;
  }

  if (KernelObj->isLambda()) {
    for (const LambdaCapture &LC : KernelObj->captures())
      if (LC.capturesThis() && LC.isImplicit()) {
        Diag(LC.getLocation(), diag::err_implicit_this_capture);
        Diag(CallLoc.getBegin(), diag::note_used_here);
        KernelFunc->setInvalidDecl();
      }
  }

  // check that calling kernel conforms to spec
  QualType KernelParamTy = KernelFunc->getParamDecl(0)->getType();
  if (KernelParamTy->isReferenceType()) {
    // passing by reference, so emit warning if not using SYCL 2020
    if (LangOpts.SYCLVersion < 2020)
      Diag(KernelFunc->getLocation(), diag::warn_sycl_pass_by_reference_future);
  } else {
    // passing by value.  emit warning if using SYCL 2020 or greater
    if (LangOpts.SYCLVersion > 2017)
      Diag(KernelFunc->getLocation(), diag::warn_sycl_pass_by_value_deprecated);
  }

  // Do not visit invalid kernel object.
  if (KernelObj->isInvalidDecl())
    return;

  SyclKernelDecompMarker DecompMarker(*this);
  SyclKernelFieldChecker FieldChecker(*this);
  SyclKernelUnionChecker UnionChecker(*this);

  bool IsSIMDKernel = isESIMDKernelType(KernelObj);
  SyclKernelArgsSizeChecker ArgsSizeChecker(*this, Args[0]->getExprLoc(),
                                            IsSIMDKernel);

  KernelObjVisitor Visitor{*this};
  SYCLKernelNameTypeVisitor KernelNameTypeVisitor(*this, Args[0]->getExprLoc(),
                                                  KernelNameType);

  DiagnosingSYCLKernel = true;

  // Emit diagnostics for SYCL device kernels only
  if (LangOpts.SYCLIsDevice)
    KernelNameTypeVisitor.Visit(KernelNameType);
  Visitor.VisitRecordBases(KernelObj, FieldChecker, UnionChecker, DecompMarker);
  Visitor.VisitRecordFields(KernelObj, FieldChecker, UnionChecker,
                            DecompMarker);
  // ArgSizeChecker needs to happen after DecompMarker has completed, since it
  // cares about the decomp attributes. DecompMarker cannot run before the
  // others, since it counts on the FieldChecker to make sure it is visiting
  // valid arrays/etc. Thus, ArgSizeChecker has its own visitation.
  if (FieldChecker.isValid() && UnionChecker.isValid()) {
    Visitor.VisitRecordBases(KernelObj, ArgsSizeChecker);
    Visitor.VisitRecordFields(KernelObj, ArgsSizeChecker);
  }
  DiagnosingSYCLKernel = false;
  // Set the kernel function as invalid, if any of the checkers fail validation.
  if (!FieldChecker.isValid() || !UnionChecker.isValid() ||
      !KernelNameTypeVisitor.isValid())
    KernelFunc->setInvalidDecl();
}

// Generates the OpenCL kernel using KernelCallerFunc (kernel caller
// function) defined is SYCL headers.
// Generated OpenCL kernel contains the body of the kernel caller function,
// receives OpenCL like parameters and additionally does some manipulation to
// initialize captured lambda/functor fields with these parameters.
// SYCL runtime marks kernel caller function with sycl_kernel attribute.
// To be able to generate OpenCL kernel from KernelCallerFunc we put
// the following requirements to the function which SYCL runtime can mark with
// sycl_kernel attribute:
//   - Must be template function with at least two template parameters.
//     First parameter must represent "unique kernel name"
//     Second parameter must be the function object type
//   - Must have only one function parameter - function object.
//
// Example of kernel caller function:
//   template <typename KernelName, typename KernelType/*, ...*/>
//   __attribute__((sycl_kernel)) void kernel_caller_function(KernelType
//                                                            KernelFuncObj) {
//     KernelFuncObj();
//   }
//
//
void Sema::ConstructOpenCLKernel(FunctionDecl *KernelCallerFunc,
                                 MangleContext &MC) {
  // The first argument to the KernelCallerFunc is the lambda object.
  const CXXRecordDecl *KernelObj = getKernelObjectType(KernelCallerFunc);
  assert(KernelObj && "invalid kernel caller");

  // Do not visit invalid kernel object.
  if (KernelObj->isInvalidDecl())
    return;

  bool IsSIMDKernel = isESIMDKernelType(KernelObj);

  // Calculate both names, since Integration headers need both.
  std::string CalculatedName, StableName;
  std::tie(CalculatedName, StableName) =
      constructKernelName(*this, KernelCallerFunc, MC);
  StringRef KernelName(getLangOpts().SYCLUnnamedLambda ? StableName
                                                       : CalculatedName);
  SyclKernelDeclCreator kernel_decl(*this, KernelName, KernelObj->getLocation(),
                                    KernelCallerFunc->isInlined(),
                                    IsSIMDKernel);
  SyclKernelBodyCreator kernel_body(*this, kernel_decl, KernelObj,
                                    KernelCallerFunc);
  SyclKernelIntHeaderCreator int_header(
      *this, getSyclIntegrationHeader(), KernelObj,
      calculateKernelNameType(Context, KernelCallerFunc), KernelName,
      StableName, KernelCallerFunc);

  KernelObjVisitor Visitor{*this};
  Visitor.VisitRecordBases(KernelObj, kernel_decl, kernel_body, int_header);
  Visitor.VisitRecordFields(KernelObj, kernel_decl, kernel_body, int_header);
}

// This function marks all the callees of explicit SIMD kernel
// with !sycl_explicit_simd. We want to have different semantics
// for functions that are called from SYCL and E-SIMD contexts.
// Later, functions marked with !sycl_explicit_simd will be cloned
// to maintain two different semantics.
void Sema::MarkSyclSimd() {
  for (Decl *D : syclDeviceDecls())
    if (auto SYCLKernel = dyn_cast<FunctionDecl>(D))
      if (SYCLKernel->hasAttr<SYCLSimdAttr>()) {
        MarkDeviceFunction Marker(*this);
        Marker.SYCLCG.addToCallGraph(getASTContext().getTranslationUnitDecl());
        llvm::SmallPtrSet<FunctionDecl *, 10> VisitedSet;
        Marker.CollectKernelSet(SYCLKernel, SYCLKernel, VisitedSet);
        for (const auto &elt : Marker.KernelSet) {
          if (FunctionDecl *Def = elt->getDefinition())
            if (!Def->hasAttr<SYCLSimdAttr>())
              Def->addAttr(SYCLSimdAttr::CreateImplicit(getASTContext()));
        }
      }
}

void Sema::MarkDevice(void) {
  // Create the call graph so we can detect recursion and check the validity
  // of new operator overrides. Add the kernel function itself in case
  // it is recursive.
  MarkDeviceFunction Marker(*this);
  Marker.SYCLCG.addToCallGraph(getASTContext().getTranslationUnitDecl());

  // Iterate through SYCL_EXTERNAL functions and add them to the device decls.
  for (const auto &entry : *Marker.SYCLCG.getRoot()) {
    if (auto *FD = dyn_cast<FunctionDecl>(entry.Callee->getDecl())) {
      if (FD->hasAttr<SYCLDeviceAttr>() && !FD->hasAttr<SYCLKernelAttr>() &&
          FD->hasBody())
        addSyclDeviceDecl(FD);
    }
  }

  for (Decl *D : syclDeviceDecls()) {
    if (auto SYCLKernel = dyn_cast<FunctionDecl>(D)) {
      llvm::SmallPtrSet<FunctionDecl *, 10> VisitedSet;
      Marker.CollectKernelSet(SYCLKernel, SYCLKernel, VisitedSet);

      // Let's propagate attributes from device functions to a SYCL kernels
      llvm::SmallPtrSet<Attr *, 4> Attrs;
      // This function collects all kernel attributes which might be applied to
      // a device functions, but need to be propagated down to callers, i.e.
      // SYCL kernels
      FunctionDecl *KernelBody =
          Marker.CollectPossibleKernelAttributes(SYCLKernel, Attrs);

      for (auto *A : Attrs) {
        switch (A->getKind()) {
        case attr::Kind::IntelReqdSubGroupSize: {
          auto *Attr = cast<IntelReqdSubGroupSizeAttr>(A);
          const auto *KBSimdAttr =
              KernelBody ? KernelBody->getAttr<SYCLSimdAttr>() : nullptr;
          if (auto *Existing =
                  SYCLKernel->getAttr<IntelReqdSubGroupSizeAttr>()) {
            if (getIntExprValue(Existing->getValue(), getASTContext()) !=
                getIntExprValue(Attr->getValue(), getASTContext())) {
              Diag(SYCLKernel->getLocation(),
                   diag::err_conflicting_sycl_kernel_attributes);
              Diag(Existing->getLocation(), diag::note_conflicting_attribute);
              Diag(Attr->getLocation(), diag::note_conflicting_attribute);
              SYCLKernel->setInvalidDecl();
            }
          } else if (KBSimdAttr && (getIntExprValue(Attr->getValue(),
                                                    getASTContext()) != 1)) {
            reportConflictingAttrs(*this, KernelBody, KBSimdAttr, Attr);
          } else {
            SYCLKernel->addAttr(A);
          }
          break;
        }
        case attr::Kind::ReqdWorkGroupSize: {
          auto *Attr = cast<ReqdWorkGroupSizeAttr>(A);
          if (auto *Existing = SYCLKernel->getAttr<ReqdWorkGroupSizeAttr>()) {
            if (Existing->getXDim() != Attr->getXDim() ||
                Existing->getYDim() != Attr->getYDim() ||
                Existing->getZDim() != Attr->getZDim()) {
              Diag(SYCLKernel->getLocation(),
                   diag::err_conflicting_sycl_kernel_attributes);
              Diag(Existing->getLocation(), diag::note_conflicting_attribute);
              Diag(Attr->getLocation(), diag::note_conflicting_attribute);
              SYCLKernel->setInvalidDecl();
            }
          } else if (auto *Existing =
                         SYCLKernel->getAttr<SYCLIntelMaxWorkGroupSizeAttr>()) {
            if (Existing->getXDim() < Attr->getXDim() ||
                Existing->getYDim() < Attr->getYDim() ||
                Existing->getZDim() < Attr->getZDim()) {
              Diag(SYCLKernel->getLocation(),
                   diag::err_conflicting_sycl_kernel_attributes);
              Diag(Existing->getLocation(), diag::note_conflicting_attribute);
              Diag(Attr->getLocation(), diag::note_conflicting_attribute);
              SYCLKernel->setInvalidDecl();
            } else {
              SYCLKernel->addAttr(A);
            }
          } else {
            SYCLKernel->addAttr(A);
          }
          break;
        }
        case attr::Kind::SYCLIntelMaxWorkGroupSize: {
          auto *Attr = cast<SYCLIntelMaxWorkGroupSizeAttr>(A);
          if (auto *Existing = SYCLKernel->getAttr<ReqdWorkGroupSizeAttr>()) {
            if (Existing->getXDim() > Attr->getXDim() ||
                Existing->getYDim() > Attr->getYDim() ||
                Existing->getZDim() > Attr->getZDim()) {
              Diag(SYCLKernel->getLocation(),
                   diag::err_conflicting_sycl_kernel_attributes);
              Diag(Existing->getLocation(), diag::note_conflicting_attribute);
              Diag(Attr->getLocation(), diag::note_conflicting_attribute);
              SYCLKernel->setInvalidDecl();
            } else {
              SYCLKernel->addAttr(A);
            }
          } else {
            SYCLKernel->addAttr(A);
          }
          break;
        }
        case attr::Kind::SYCLIntelKernelArgsRestrict:
        case attr::Kind::SYCLIntelNumSimdWorkItems:
        case attr::Kind::SYCLIntelSchedulerTargetFmaxMhz:
        case attr::Kind::SYCLIntelMaxGlobalWorkDim:
        case attr::Kind::SYCLIntelNoGlobalWorkOffset:
        case attr::Kind::SYCLIntelUseStallEnableClusters:
        case attr::Kind::SYCLIntelLoopFuse:
        case attr::Kind::SYCLSimd: {
          if ((A->getKind() == attr::Kind::SYCLSimd) && KernelBody &&
              !KernelBody->getAttr<SYCLSimdAttr>()) {
            // Usual kernel can't call ESIMD functions.
            Diag(KernelBody->getLocation(),
                 diag::err_sycl_function_attribute_mismatch)
                << A;
            Diag(A->getLocation(), diag::note_attribute);
            KernelBody->setInvalidDecl();
          } else
            SYCLKernel->addAttr(A);
          break;
        }
        // TODO: vec_len_hint should be handled here
        default:
          // Seeing this means that CollectPossibleKernelAttributes was
          // updated while this switch wasn't...or something went wrong
          llvm_unreachable("Unexpected attribute was collected by "
                           "CollectPossibleKernelAttributes");
        }
      }
    }
  }
  for (const auto &elt : Marker.KernelSet) {
    if (FunctionDecl *Def = elt->getDefinition())
      Marker.TraverseStmt(Def->getBody());
  }
}

// -----------------------------------------------------------------------------
// SYCL device specific diagnostics implementation
// -----------------------------------------------------------------------------

Sema::SemaDiagnosticBuilder Sema::SYCLDiagIfDeviceCode(SourceLocation Loc,
                                                       unsigned DiagID) {
  assert(getLangOpts().SYCLIsDevice &&
         "Should only be called during SYCL compilation");
  FunctionDecl *FD = dyn_cast<FunctionDecl>(getCurLexicalContext());
  SemaDiagnosticBuilder::Kind DiagKind = [this, FD] {
    if (DiagnosingSYCLKernel)
      return SemaDiagnosticBuilder::K_ImmediateWithCallStack;
    if (!FD)
      return SemaDiagnosticBuilder::K_Nop;
    if (getEmissionStatus(FD) == Sema::FunctionEmissionStatus::Emitted)
      return SemaDiagnosticBuilder::K_ImmediateWithCallStack;
    return SemaDiagnosticBuilder::K_Deferred;
  }();
  return SemaDiagnosticBuilder(DiagKind, Loc, DiagID, FD, *this);
}

bool Sema::checkSYCLDeviceFunction(SourceLocation Loc, FunctionDecl *Callee) {
  assert(getLangOpts().SYCLIsDevice &&
         "Should only be called during SYCL compilation");
  assert(Callee && "Callee may not be null.");

  // Errors in unevaluated context don't need to be generated,
  // so we can safely skip them.
  if (isUnevaluatedContext() || isConstantEvaluated())
    return true;

  FunctionDecl *Caller = dyn_cast<FunctionDecl>(getCurLexicalContext());

  if (!Caller)
    return true;

  SemaDiagnosticBuilder::Kind DiagKind = SemaDiagnosticBuilder::K_Nop;

  // TODO Set DiagKind to K_Immediate/K_Deferred to emit diagnostics for Callee

  SemaDiagnosticBuilder(DiagKind, Loc, diag::err_sycl_restrict, Caller, *this)
      << Sema::KernelCallUndefinedFunction;
  SemaDiagnosticBuilder(DiagKind, Callee->getLocation(), diag::note_previous_decl,
                    Caller, *this)
      << Callee;

  return DiagKind != SemaDiagnosticBuilder::K_Immediate &&
         DiagKind != SemaDiagnosticBuilder::K_ImmediateWithCallStack;
}

void Sema::finalizeSYCLDelayedAnalysis(const FunctionDecl *Caller,
                                       const FunctionDecl *Callee,
                                       SourceLocation Loc) {
  // Somehow an unspecialized template appears to be in callgraph or list of
  // device functions. We don't want to emit diagnostic here.
  if (Callee->getTemplatedKind() == FunctionDecl::TK_FunctionTemplate)
    return;

  Callee = Callee->getMostRecentDecl();
  bool HasAttr =
      Callee->hasAttr<SYCLDeviceAttr>() || Callee->hasAttr<SYCLKernelAttr>();

  // Disallow functions with neither definition nor SYCL_EXTERNAL mark
  bool NotDefinedNoAttr = !Callee->isDefined() && !HasAttr;

  if (NotDefinedNoAttr && !Callee->getBuiltinID()) {
    Diag(Loc, diag::err_sycl_restrict)
        << Sema::KernelCallUndefinedFunction;
    Diag(Callee->getLocation(), diag::note_previous_decl) << Callee;
    Diag(Caller->getLocation(), diag::note_called_by) << Caller;
  }
}

bool Sema::checkAllowedSYCLInitializer(VarDecl *VD, bool CheckValueDependent) {
  assert(getLangOpts().SYCLIsDevice &&
         "Should only be called during SYCL compilation");

  if (VD->isInvalidDecl() || !VD->hasInit() || !VD->hasGlobalStorage())
    return true;

  const Expr *Init = VD->getInit();
  bool ValueDependent = CheckValueDependent && Init->isValueDependent();
  bool isConstantInit =
      Init && !ValueDependent && Init->isConstantInitializer(Context, false);
  if (!VD->isConstexpr() && Init && !ValueDependent && !isConstantInit)
    return false;

  return true;
}

// -----------------------------------------------------------------------------
// Integration header functionality implementation
// -----------------------------------------------------------------------------

/// Returns a string ID of given parameter kind - used in header
/// emission.
static const char *paramKind2Str(KernelParamKind K) {
#define CASE(x)                                                                \
  case SYCLIntegrationHeader::kind_##x:                                        \
    return "kind_" #x
  switch (K) {
    CASE(accessor);
    CASE(std_layout);
    CASE(sampler);
    CASE(pointer);
  default:
    return "<ERROR>";
  }
#undef CASE
}

// Emits forward declarations of classes and template classes on which
// declaration of given type depends.
// For example, consider SimpleVadd
// class specialization in parallel_for below:
//
//   template <typename T1, unsigned int N, typename ... T2>
//   class SimpleVadd;
//   ...
//   template <unsigned int N, typename T1, typename ... T2>
//   void simple_vadd(const std::array<T1, N>& VA, const std::array<T1, N>&
//   VB,
//     std::array<T1, N>& VC, int param, T2 ... varargs) {
//     ...
//     deviceQueue.submit([&](cl::sycl::handler& cgh) {
//       ...
//       cgh.parallel_for<class SimpleVadd<T1, N, T2...>>(...)
//       ...
//     }
//     ...
//   }
//   ...
//   class MyClass {...};
//   template <typename T> class MyInnerTmplClass { ... }
//   template <typename T> class MyTmplClass { ... }
//   ...
//   MyClass *c = new MyClass();
//   MyInnerTmplClass<MyClass**> c1(&c);
//   simple_vadd(A, B, C, 5, 'a', 1.f,
//     new MyTmplClass<MyInnerTmplClass<MyClass**>>(c1));
//
// it will generate the following forward declarations:
//   class MyClass;
//   template <typename T> class MyInnerTmplClass;
//   template <typename T> class MyTmplClass;
//   template <typename T1, unsigned int N, typename ...T2> class SimpleVadd;
//
class SYCLFwdDeclEmitter
    : public TypeVisitor<SYCLFwdDeclEmitter>,
      public ConstTemplateArgumentVisitor<SYCLFwdDeclEmitter> {
  using InnerTypeVisitor = TypeVisitor<SYCLFwdDeclEmitter>;
  using InnerTemplArgVisitor = ConstTemplateArgumentVisitor<SYCLFwdDeclEmitter>;
  raw_ostream &OS;
  llvm::SmallPtrSet<const NamedDecl *, 4> Printed;
  PrintingPolicy Policy;

  void printForwardDecl(NamedDecl *D) {
    // wrap the declaration into namespaces if needed
    unsigned NamespaceCnt = 0;
    std::string NSStr = "";
    const DeclContext *DC = D->getDeclContext();

    while (DC) {
      const auto *NS = dyn_cast_or_null<NamespaceDecl>(DC);

      if (!NS)
        break;

      ++NamespaceCnt;
      const StringRef NSInlinePrefix = NS->isInline() ? "inline " : "";
      NSStr.insert(
          0,
          Twine(NSInlinePrefix + "namespace " + NS->getName() + " { ").str());
      DC = NS->getDeclContext();
    }
    OS << NSStr;
    if (NamespaceCnt > 0)
      OS << "\n";

    D->print(OS, Policy);

    if (const auto *ED = dyn_cast<EnumDecl>(D)) {
      QualType T = ED->getIntegerType();
      // Backup since getIntegerType() returns null for enum forward
      // declaration with no fixed underlying type
      if (T.isNull())
        T = ED->getPromotionType();
      OS << " : " << T.getAsString();
    }

    OS << ";\n";

    // print closing braces for namespaces if needed
    for (unsigned I = 0; I < NamespaceCnt; ++I)
      OS << "}";
    if (NamespaceCnt > 0)
      OS << "\n";
  }

  // Checks if we've already printed forward declaration and prints it if not.
  void checkAndEmitForwardDecl(NamedDecl *D) {
    if (Printed.insert(D).second)
      printForwardDecl(D);
  }

  void VisitTemplateArgs(ArrayRef<TemplateArgument> Args) {
    for (size_t I = 0, E = Args.size(); I < E; ++I)
      Visit(Args[I]);
  }

public:
  SYCLFwdDeclEmitter(raw_ostream &OS, LangOptions LO) : OS(OS), Policy(LO) {
    Policy.adjustForCPlusPlusFwdDecl();
    Policy.SuppressTypedefs = true;
    Policy.SuppressUnwrittenScope = true;
  }

  void Visit(QualType T) {
    if (T.isNull())
      return;
    InnerTypeVisitor::Visit(T.getTypePtr());
  }

  void Visit(const TemplateArgument &TA) {
    if (TA.isNull())
      return;
    InnerTemplArgVisitor::Visit(TA);
  }

  void VisitPointerType(const PointerType *T) {
    // Peel off the pointer types.
    QualType PT = T->getPointeeType();
    while (PT->isPointerType())
      PT = PT->getPointeeType();
    Visit(PT);
  }

  void VisitTagType(const TagType *T) {
    TagDecl *TD = T->getDecl();
    if (const auto *TSD = dyn_cast<ClassTemplateSpecializationDecl>(TD)) {
      // - first, recurse into template parameters and emit needed forward
      //   declarations
      ArrayRef<TemplateArgument> Args = TSD->getTemplateArgs().asArray();
      VisitTemplateArgs(Args);
      // - second, emit forward declaration for the template class being
      //   specialized
      ClassTemplateDecl *CTD = TSD->getSpecializedTemplate();
      assert(CTD && "template declaration must be available");

      checkAndEmitForwardDecl(CTD);
      return;
    }
    checkAndEmitForwardDecl(TD);
  }

  void VisitTypeTemplateArgument(const TemplateArgument &TA) {
    QualType T = TA.getAsType();
    Visit(T);
  }

  void VisitIntegralTemplateArgument(const TemplateArgument &TA) {
    QualType T = TA.getIntegralType();
    if (const EnumType *ET = T->getAs<EnumType>())
      VisitTagType(ET);
  }

  void VisitTemplateTemplateArgument(const TemplateArgument &TA) {
    // recursion is not required, since the maximum possible nesting level
    // equals two for template argument
    //
    // for example:
    //   template <typename T> class Bar;
    //   template <template <typename> class> class Baz;
    //   template <template <template <typename> class> class T>
    //   class Foo;
    //
    // The Baz is a template class. The Baz<Bar> is a class. The class Foo
    // should be specialized with template class, not a class. The correct
    // specialization of template class Foo is Foo<Baz>. The incorrect
    // specialization of template class Foo is Foo<Baz<Bar>>. In this case
    // template class Foo specialized by class Baz<Bar>, not a template
    // class template <template <typename> class> class T as it should.
    TemplateDecl *TD = TA.getAsTemplate().getAsTemplateDecl();
    assert(TD && "template declaration must be available");
    TemplateParameterList *TemplateParams = TD->getTemplateParameters();
    for (NamedDecl *P : *TemplateParams) {
      // If template template parameter type has an enum value template
      // parameter, forward declaration of enum type is required. Only enum
      // values (not types) need to be handled. For example, consider the
      // following kernel name type:
      //
      // template <typename EnumTypeOut, template <EnumValueIn EnumValue,
      // typename TypeIn> class T> class Foo;
      //
      // The correct specialization for Foo (with enum type) is:
      // Foo<EnumTypeOut, Baz>, where Baz is a template class.
      //
      // Therefore the forward class declarations generated in the
      // integration header are:
      // template <EnumValueIn EnumValue, typename TypeIn> class Baz;
      // template <typename EnumTypeOut, template <EnumValueIn EnumValue,
      // typename EnumTypeIn> class T> class Foo;
      //
      // This requires the following enum forward declarations:
      // enum class EnumTypeOut : int; (Used to template Foo)
      // enum class EnumValueIn : int; (Used to template Baz)
      if (NonTypeTemplateParmDecl *TemplateParam =
              dyn_cast<NonTypeTemplateParmDecl>(P))
        if (const EnumType *ET = TemplateParam->getType()->getAs<EnumType>())
          VisitTagType(ET);
    }
    checkAndEmitForwardDecl(TD);
  }

  void VisitPackTemplateArgument(const TemplateArgument &TA) {
    VisitTemplateArgs(TA.getPackAsArray());
  }
};

class SYCLKernelNameTypePrinter
    : public TypeVisitor<SYCLKernelNameTypePrinter>,
      public ConstTemplateArgumentVisitor<SYCLKernelNameTypePrinter> {
  using InnerTypeVisitor = TypeVisitor<SYCLKernelNameTypePrinter>;
  using InnerTemplArgVisitor =
      ConstTemplateArgumentVisitor<SYCLKernelNameTypePrinter>;
  raw_ostream &OS;
  PrintingPolicy &Policy;

  void printTemplateArgs(ArrayRef<TemplateArgument> Args) {
    for (size_t I = 0, E = Args.size(); I < E; ++I) {
      const TemplateArgument &Arg = Args[I];
      // If argument is an empty pack argument, skip printing comma and
      // argument.
      if (Arg.getKind() == TemplateArgument::ArgKind::Pack && !Arg.pack_size())
        continue;

      if (I)
        OS << ", ";

      Visit(Arg);
    }
  }

  void VisitQualifiers(Qualifiers Quals) {
    Quals.print(OS, Policy, /*appendSpaceIfNotEmpty*/ true);
  }

public:
  SYCLKernelNameTypePrinter(raw_ostream &OS, PrintingPolicy &Policy)
      : OS(OS), Policy(Policy) {}

  void Visit(QualType T) {
    if (T.isNull())
      return;

    QualType CT = T.getCanonicalType();
    VisitQualifiers(CT.getQualifiers());

    InnerTypeVisitor::Visit(CT.getTypePtr());
  }

  void VisitType(const Type *T) {
    OS << QualType::getAsString(T, Qualifiers(), Policy);
  }

  void Visit(const TemplateArgument &TA) {
    if (TA.isNull())
      return;
    InnerTemplArgVisitor::Visit(TA);
  }

  void VisitTagType(const TagType *T) {
    TagDecl *RD = T->getDecl();
    if (const auto *TSD = dyn_cast<ClassTemplateSpecializationDecl>(RD)) {

      // Print template class name
      TSD->printQualifiedName(OS, Policy, /*WithGlobalNsPrefix*/ true);

      ArrayRef<TemplateArgument> Args = TSD->getTemplateArgs().asArray();
      OS << "<";
      printTemplateArgs(Args);
      OS << ">";

      return;
    }
    // TODO: Next part of code results in printing of "class" keyword before
    // class name in case if kernel name doesn't belong to some namespace. It
    // seems if we don't print it, the integration header still represents valid
    // c++ code. Probably we don't need to print it at all.
    if (RD->getDeclContext()->isFunctionOrMethod()) {
      OS << QualType::getAsString(T, Qualifiers(), Policy);
      return;
    }

    const NamespaceDecl *NS = dyn_cast<NamespaceDecl>(RD->getDeclContext());
    RD->printQualifiedName(OS, Policy, !(NS && NS->isAnonymousNamespace()));
  }

  void VisitTemplateArgument(const TemplateArgument &TA) {
    TA.print(Policy, OS);
  }

  void VisitTypeTemplateArgument(const TemplateArgument &TA) {
    Policy.SuppressTagKeyword = true;
    QualType T = TA.getAsType();
    Visit(T);
    Policy.SuppressTagKeyword = false;
  }

  void VisitIntegralTemplateArgument(const TemplateArgument &TA) {
    QualType T = TA.getIntegralType();
    if (const EnumType *ET = T->getAs<EnumType>()) {
      const llvm::APSInt &Val = TA.getAsIntegral();
      OS << "static_cast<";
      ET->getDecl()->printQualifiedName(OS, Policy,
                                        /*WithGlobalNsPrefix*/ true);
      OS << ">(" << Val << ")";
    } else {
      TA.print(Policy, OS);
    }
  }

  void VisitTemplateTemplateArgument(const TemplateArgument &TA) {
    TemplateDecl *TD = TA.getAsTemplate().getAsTemplateDecl();
    TD->printQualifiedName(OS, Policy);
  }

  void VisitPackTemplateArgument(const TemplateArgument &TA) {
    printTemplateArgs(TA.getPackAsArray());
  }
};

void SYCLIntegrationHeader::emit(raw_ostream &O) {
  O << "// This is auto-generated SYCL integration header.\n";
  O << "\n";

  O << "#include <CL/sycl/detail/defines_elementary.hpp>\n";
  O << "#include <CL/sycl/detail/kernel_desc.hpp>\n";

  O << "\n";

  LangOptions LO;
  PrintingPolicy Policy(LO);
  Policy.SuppressTypedefs = true;
  Policy.SuppressUnwrittenScope = true;
  SYCLFwdDeclEmitter FwdDeclEmitter(O, S.getLangOpts());

  if (SpecConsts.size() > 0) {
    O << "// Forward declarations of templated spec constant types:\n";
    for (const auto &SC : SpecConsts)
      FwdDeclEmitter.Visit(SC.first);
    O << "\n";

    // Remove duplicates.
    std::sort(SpecConsts.begin(), SpecConsts.end(),
              [](const SpecConstID &SC1, const SpecConstID &SC2) {
                // Sort by string IDs for stable spec consts order in the
                // header.
                return SC1.second.compare(SC2.second) < 0;
              });
    SpecConstID *End =
        std::unique(SpecConsts.begin(), SpecConsts.end(),
                    [](const SpecConstID &SC1, const SpecConstID &SC2) {
                      // Here can do faster comparison of types.
                      return SC1.first == SC2.first;
                    });

    O << "// Specialization constants IDs:\n";
    for (const auto &P : llvm::make_range(SpecConsts.begin(), End)) {
      O << "template <> struct sycl::detail::SpecConstantInfo<";
      SYCLKernelNameTypePrinter Printer(O, Policy);
      Printer.Visit(P.first);
      O << "> {\n";
      O << "  static constexpr const char* getName() {\n";
      O << "    return \"" << P.second << "\";\n";
      O << "  }\n";
      O << "};\n";
    }
  }

  if (!UnnamedLambdaSupport) {
    O << "// Forward declarations of templated kernel function types:\n";
    for (const KernelDesc &K : KernelDescs)
      FwdDeclEmitter.Visit(K.NameType);
  }
  O << "\n";

  O << "__SYCL_INLINE_NAMESPACE(cl) {\n";
  O << "namespace sycl {\n";
  O << "namespace detail {\n";

  O << "\n";

  O << "// names of all kernels defined in the corresponding source\n";
  O << "static constexpr\n";
  O << "const char* const kernel_names[] = {\n";

  for (unsigned I = 0; I < KernelDescs.size(); I++) {
    O << "  \"" << KernelDescs[I].Name << "\"";

    if (I < KernelDescs.size() - 1)
      O << ",";
    O << "\n";
  }
  O << "};\n\n";

  O << "// array representing signatures of all kernels defined in the\n";
  O << "// corresponding source\n";
  O << "static constexpr\n";
  O << "const kernel_param_desc_t kernel_signatures[] = {\n";

  for (unsigned I = 0; I < KernelDescs.size(); I++) {
    auto &K = KernelDescs[I];
    O << "  //--- " << K.Name << "\n";

    for (const auto &P : K.Params) {
      std::string TyStr = paramKind2Str(P.Kind);
      O << "  { kernel_param_kind_t::" << TyStr << ", ";
      O << P.Info << ", " << P.Offset << " },\n";
    }
    O << "\n";
  }
  O << "};\n\n";

  O << "// Specializations of KernelInfo for kernel function types:\n";
  unsigned CurStart = 0;

  for (const KernelDesc &K : KernelDescs) {
    const size_t N = K.Params.size();
    if (UnnamedLambdaSupport) {
      O << "template <> struct KernelInfoData<";
      O << "'" << K.StableName.front();
      for (char c : StringRef(K.StableName).substr(1))
        O << "', '" << c;
      O << "'> {\n";
    } else {
      O << "template <> struct KernelInfo<";
      SYCLKernelNameTypePrinter Printer(O, Policy);
      Printer.Visit(K.NameType);
      O << "> {\n";
    }
    O << "  __SYCL_DLL_LOCAL\n";
    O << "  static constexpr const char* getName() { return \"" << K.Name
      << "\"; }\n";
    O << "  __SYCL_DLL_LOCAL\n";
    O << "  static constexpr unsigned getNumParams() { return " << N << "; }\n";
    O << "  __SYCL_DLL_LOCAL\n";
    O << "  static constexpr const kernel_param_desc_t& ";
    O << "getParamDesc(unsigned i) {\n";
    O << "    return kernel_signatures[i+" << CurStart << "];\n";
    O << "  }\n";
    O << "  __SYCL_DLL_LOCAL\n";
    O << "  static constexpr bool isESIMD() { return " << K.IsESIMDKernel
      << "; }\n";
    O << "  __SYCL_DLL_LOCAL\n";
    O << "  static constexpr bool callsThisItem() { return ";
    O << K.CallsThisItem << "; }\n";
    O << "};\n";
    CurStart += N;
  }
  O << "\n";
  O << "} // namespace detail\n";
  O << "} // namespace sycl\n";
  O << "} // __SYCL_INLINE_NAMESPACE(cl)\n";
  O << "\n";
}

bool SYCLIntegrationHeader::emit(const StringRef &IntHeaderName) {
  if (IntHeaderName.empty())
    return false;
  int IntHeaderFD = 0;
  std::error_code EC =
      llvm::sys::fs::openFileForWrite(IntHeaderName, IntHeaderFD);
  if (EC) {
    llvm::errs() << "Error: " << EC.message() << "\n";
    // compilation will fail on absent include file - don't need to fail here
    return false;
  }
  llvm::raw_fd_ostream Out(IntHeaderFD, true /*close in destructor*/);
  emit(Out);
  return true;
}

void SYCLIntegrationHeader::startKernel(StringRef KernelName,
                                        QualType KernelNameType,
                                        StringRef KernelStableName,
                                        SourceLocation KernelLocation,
                                        bool IsESIMDKernel) {
  KernelDescs.resize(KernelDescs.size() + 1);
  KernelDescs.back().Name = std::string(KernelName);
  KernelDescs.back().NameType = KernelNameType;
  KernelDescs.back().StableName = std::string(KernelStableName);
  KernelDescs.back().KernelLocation = KernelLocation;
  KernelDescs.back().IsESIMDKernel = IsESIMDKernel;
}

void SYCLIntegrationHeader::addParamDesc(kernel_param_kind_t Kind, int Info,
                                         unsigned Offset) {
  auto *K = getCurKernelDesc();
  assert(K && "no kernels");
  K->Params.push_back(KernelParamDesc());
  KernelParamDesc &PD = K->Params.back();
  PD.Kind = Kind;
  PD.Info = Info;
  PD.Offset = Offset;
}

void SYCLIntegrationHeader::endKernel() {
  // nop for now
}

void SYCLIntegrationHeader::addSpecConstant(StringRef IDName, QualType IDType) {
  SpecConsts.emplace_back(std::make_pair(IDType, IDName.str()));
}

void SYCLIntegrationHeader::setCallsThisItem(bool B) {
  KernelDesc *K = getCurKernelDesc();
  assert(K && "no kernels");
  K->CallsThisItem = B;
}

SYCLIntegrationHeader::SYCLIntegrationHeader(DiagnosticsEngine &_Diag,
                                             bool _UnnamedLambdaSupport,
                                             Sema &_S)
    : UnnamedLambdaSupport(_UnnamedLambdaSupport), S(_S) {}

// -----------------------------------------------------------------------------
// Utility class methods
// -----------------------------------------------------------------------------

bool Util::isSyclAccessorType(const QualType &Ty) {
  return isSyclType(Ty, "accessor", true /*Tmpl*/);
}

bool Util::isSyclSamplerType(const QualType &Ty) {
  return isSyclType(Ty, "sampler");
}

bool Util::isSyclStreamType(const QualType &Ty) {
  return isSyclType(Ty, "stream");
}

bool Util::isSyclHalfType(const QualType &Ty) {
  const StringRef &Name = "half";
  std::array<DeclContextDesc, 5> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "detail"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "half_impl"},
      Util::DeclContextDesc{Decl::Kind::CXXRecord, Name}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::isSyclSpecConstantType(const QualType &Ty) {
  const StringRef &Name = "spec_constant";
  std::array<DeclContextDesc, 5> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "ONEAPI"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "experimental"},
      Util::DeclContextDesc{Decl::Kind::ClassTemplateSpecialization, Name}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::isSyclBufferLocationType(const QualType &Ty) {
  const StringRef &PropertyName = "buffer_location";
  const StringRef &InstanceName = "instance";
  std::array<DeclContextDesc, 6> Scopes = {
      Util::DeclContextDesc{Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{Decl::Kind::Namespace, "INTEL"},
      Util::DeclContextDesc{Decl::Kind::Namespace, "property"},
      Util::DeclContextDesc{Decl::Kind::CXXRecord, PropertyName},
      Util::DeclContextDesc{Decl::Kind::ClassTemplateSpecialization,
                            InstanceName}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::isSyclType(const QualType &Ty, StringRef Name, bool Tmpl) {
  Decl::Kind ClassDeclKind =
      Tmpl ? Decl::Kind::ClassTemplateSpecialization : Decl::Kind::CXXRecord;
  std::array<DeclContextDesc, 3> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{ClassDeclKind, Name}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::isSyclFunction(const FunctionDecl *FD, StringRef Name) {
  if (!FD->isFunctionOrMethod() || !FD->getIdentifier() ||
      FD->getName().empty() || Name != FD->getName())
    return false;

  const DeclContext *DC = FD->getDeclContext();
  if (DC->isTranslationUnit())
    return false;

  std::array<DeclContextDesc, 2> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"}};
  return matchContext(DC, Scopes);
}

bool Util::isAccessorPropertyListType(const QualType &Ty) {
  const StringRef &Name = "accessor_property_list";
  std::array<DeclContextDesc, 4> Scopes = {
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "cl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "sycl"},
      Util::DeclContextDesc{clang::Decl::Kind::Namespace, "ONEAPI"},
      Util::DeclContextDesc{Decl::Kind::ClassTemplateSpecialization, Name}};
  return matchQualifiedTypeName(Ty, Scopes);
}

bool Util::matchContext(const DeclContext *Ctx,
                        ArrayRef<Util::DeclContextDesc> Scopes) {
  // The idea: check the declaration context chain starting from the item
  // itself. At each step check the context is of expected kind
  // (namespace) and name.
  StringRef Name = "";

  for (const auto &Scope : llvm::reverse(Scopes)) {
    clang::Decl::Kind DK = Ctx->getDeclKind();
    if (DK != Scope.first)
      return false;

    switch (DK) {
    case clang::Decl::Kind::ClassTemplateSpecialization:
      // ClassTemplateSpecializationDecl inherits from CXXRecordDecl
    case clang::Decl::Kind::CXXRecord:
      Name = cast<CXXRecordDecl>(Ctx)->getName();
      break;
    case clang::Decl::Kind::Namespace:
      Name = cast<NamespaceDecl>(Ctx)->getName();
      break;
    default:
      llvm_unreachable("matchContext: decl kind not supported");
    }
    if (Name != Scope.second)
      return false;
    Ctx = Ctx->getParent();
  }
  return Ctx->isTranslationUnit();
}

bool Util::matchQualifiedTypeName(const QualType &Ty,
                                  ArrayRef<Util::DeclContextDesc> Scopes) {
  const CXXRecordDecl *RecTy = Ty->getAsCXXRecordDecl();

  if (!RecTy)
    return false; // only classes/structs supported
  const auto *Ctx = cast<DeclContext>(RecTy);
  return Util::matchContext(Ctx, Scopes);
}
