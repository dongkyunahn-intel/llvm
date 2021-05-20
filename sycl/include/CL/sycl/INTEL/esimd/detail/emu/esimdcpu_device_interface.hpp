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


// Denotes the data version used by the implementation.
// Increment whenever the 'data' field interpretation within PluginOpaqueData is changed.
#define ESIMD_EMU_PLUGIN_OPAQUE_DATA_VERSION 0
/// This structure denotes a ESIMD EMU plugin-specific data returned via the
/// piextPluginGetOpaqueData PI call. Depending on the \c version field, the second \c data
/// field can be interpreted differently.
struct ESIMDEmuPluginOpaqueData {
  uintptr_t version;
  void *data;
};
// The table below shows the correspondence between the \c version
// and the contents of the \c data field:
// version == 0, data is ESIMDDeviceInterface*

ESIMDDeviceInterface *getESIMDDeviceInterface() {
  // TODO (performance) cache the interface pointer, can make a difference when
  // calling fine-grained libCM APIs through it (like memory access in a tight
  // loop)
  PluginOpaqueData *OpaqueData = nullptr;

  const plugin &EsimdPlugin =
      cl::sycl::detail::pi::getPlugin<cl::sycl::backend::esimd_cpu>();
  EsimdPlugin.call<cl::sycl::detail::PiApiKind::piextPluginGetOpaqueData>(
      nullptr, &OpaqueData);

  // First check if opaque data version is compatible. 
  if (OpaqueData->version != ESIMD_EMU_PLUGIN_OPAQUE_DATA_VERSION) {
    // NOTE: the version check should always be '!=' as layouts of different
    // versions of PluginOpaqueData is not backward compatible, unlike
    // layout of the ESIMDDeviceInterface.

    std::cerr << __FUNCTION__
              << "Opaque data returned by ESIMD Emu plugin is incompatible with"
              << "the one used in current implementation."  << std::endl
              << "Returned version : " << OpaqueData->version << std::endl
              << "Required version : " << ESIMD_EMU_PLUGIN_OPAQUE_DATA_VERSION
              << std::endl;
    throw cl::sycl::feature_not_supported();
  }
  // Opaque data version is OK, can cast the 'data' field.
  ESIMDDeviceInterface *Interface =
    reinterpret_cast<ESIMDDeviceInterface*>(OpaqueData->data);

  // Now check that device interface version is compatible. 
  if (Intf->version < ESIMD_DEVICE_INTERFACE_VERSION) {
    std::cerr << __FUNCTION__
              << "The device interface version provided from plug-in "
              << "library is behind required device interface version"
              << std::endl
              << "Found version : " << Intf->version << std::endl
              << "Required version :" << ESIMD_DEVICE_INTERFACE_VERSION
              << std::endl;
    throw cl::sycl::feature_not_supported();
  }
  return Intf;
}
