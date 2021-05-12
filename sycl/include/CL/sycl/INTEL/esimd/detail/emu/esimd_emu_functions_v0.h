//==----- esimd_emu_functions_v0.h - DPC++ Explicit SIMD API ---------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file esimd_emu_functions_v0.h
///
/// \ingroup sycl_pi_esimd_cpu

#pragma once

// Intrinsics
void (*cm_barrier_ptr)(void);
void (*cm_sbarrier_ptr)(uint);
void (*cm_fence_ptr)(void);

// libcm functionalities used for intrinsics such as
// surface/buffer/slm access
char *(*sycl_get_surface_base_ptr)(int);
char *(*__cm_emu_get_slm_ptr)(void);
void (*cm_slm_init_ptr)(size_t);
