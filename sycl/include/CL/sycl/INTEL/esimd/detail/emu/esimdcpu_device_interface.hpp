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

#include <CL/sycl/detail/plugin.hpp>
#include <cstdint>

/// This is the device interface version required (and used) by this implementation of
/// the ESIMD CPU emulator.
#define ESIMD_DEVICE_INTERFACE_VERSION 0

#ifdef _MSC_VER
// Definitions for type consistency between ESIMD_CPU and CM_EMU
typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;
#endif // _MSC_VER

struct ESIMDDeviceInterface {
  void *reserved;

  ESIMDDeviceInterface();
#include "esimd_emu_functions_v0.h"
};

struct OpaqueDataAccess {
  uintptr_t version;
  struct ESIMDDeviceInterface *interface;
};

ESIMDDeviceInterface *getESIMDDeviceInterface() {
  void *RawOpaqueDataAccess;

  const auto &esimdPlugin =
      cl::sycl::detail::pi::getPlugin<cl::sycl::backend::esimd_cpu>();
  esimdPlugin.call<cl::sycl::detail::PiApiKind::piextPluginGetOpaqueData>(
      nullptr, &RawOpaqueDataAccess);

  OpaqueDataAccess *dataAccess =
      reinterpret_cast<OpaqueDataAccess *>(RawOpaqueDataAccess);

  if (dataAccess->version != ESIMD_CPU_DEVICE_REQUIRED_VER) {
    // TODO : version < ESIMD_CPU_DEVICE_REQUIRED_VER when
    // ESIMD_CPU_DEVICE_REQUIRED_VER becomes larger than 0
    std::cerr << __FUNCTION__
              << "The device interface version provided from plug-in "
              << "library is behind required device interface version"
              << std::endl
              << "Device version : " << dataAccess->version << std::endl
              << "Required version :" << dataAccess->version << std::endl;
    throw cl::sycl::feature_not_supported();
  }
  return dataAccess->interface;
}
