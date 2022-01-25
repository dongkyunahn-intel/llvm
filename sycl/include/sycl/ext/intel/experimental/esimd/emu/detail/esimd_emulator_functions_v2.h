//==----- esimd_emulator_functions_v2.h - DPC++ Explicit SIMD API ---------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file esimd_emulator_functions_v2.h
///
/// \ingroup sycl_pi_esimd_emulator

#pragma once

// <cstdint> for 'uint32_t' type is included in upper-level device
// interface file ('esimdemu_device_interface.hpp')

// This file defines function interfaces for ESIMD CPU Emulation
// (ESIMD_EMU) to access LibCM CPU emulation functionalities from
// kernel applications running under emulation

// CM CPU Emulation Info :
// https://github.com/intel/cm-cpu-emulation

// Function pointers (*_ptr) with 'cm/__cm' prefixes correspond to
// LibCM functions with same name
// e.g.: cm_fence_ptr -> cm_fence() in LibCM

// Function pointers (*_ptr) with 'sycl_' prefix correspond to LibCM
// functions dedicated to SYCL support
// e.g.: sycl_get_surface_base_addr_ptr
// -> get_surface_base_addr(int) in LibCM

/****** DO NOT MODIFY following function pointers ******/
/****** No reordering, No renaming, No removal    ******/

// libcm functionalities used for intrinsics such as
// surface/buffer/slm access
unsigned int (*sycl_get_cm_surface_index_ptr)(void *);
void (*sycl_get_cm_buffer_params_index_ptr)(unsigned int, char **, uint32_t *,
                                            std::mutex **);
void (*sycl_get_cm_image_params_index_ptr)(unsigned int, char **, uint32_t *,
                                           uint32_t *, uint32_t *,
                                           std::mutex **);
