//==----- esimdcpu_device_interface.hpp - DPC++ Explicit SIMD API ---------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file esimdcpu_device_interface.hpp
/// Declarations for ESIMD_CPU-device specific definitions.
/// ESIMD intrinsic and LibCM functionalities required by intrinsic defined
///
/// This interface is for ESIMD intrinsic emulation implementations
/// such as slm_access to access ESIMD_CPU specific-support therefore
/// it has to be defined and shared as include directory
///
/// \ingroup sycl_pi_esimd_cpu

#pragma once

#include <cstdint>

#ifdef _MSC_VER
// Definitions for type consistency between ESIMD_CPU and CM_EMU
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;
#endif // _MSC_VER

struct ESIMDDeviceInterface {

  // Intrinsics
  virtual void mt_barrier() = 0;
  virtual void split_barrier(uint) = 0;
  virtual void fence() = 0;

  // libcm functionalities used for intrinsics such as
  // surface/buffer/slm access
  virtual char *get_surface_base(int surfaceID) = 0;
  virtual char *get_slm() = 0;
  virtual void set_slm_size(size_t size) = 0;
};
