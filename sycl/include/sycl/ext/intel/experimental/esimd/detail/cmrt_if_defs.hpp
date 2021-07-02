//==---------- cmrt_if_defs.hpp - CM-Runtime interface header file ---------==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

/// \file cmrt_if_defs.hpp
///
/// Interface definitions for esimd_cpu pi module to communitcate with
/// underlying CM emulation runtime library
///
/// \ingroup sycl_pi_esimd_cpu

#pragma once

/// CMRT Inteface Defines

#include <unordered_map>

// Base class to store common data
struct _pi_object {
  _pi_object() : RefCount{1} {}

  std::atomic<pi_uint32> RefCount;
};
struct _pi_platform {
  _pi_platform() {}

  // Keep Version information.
  std::string CmEmuVersion;
};

struct _pi_device : _pi_object {
  _pi_device(pi_platform plt) : Platform{plt} {}

  pi_platform Platform;
};

struct _pi_context : _pi_object {
  _pi_context(pi_device DeviceArg, cm_support::CmDevice *CmDeviceArg)
      : Device{DeviceArg}, CmDevicePtr{CmDeviceArg} {}

  /// One-to-one mapping between Context and Device
  pi_device Device;

  cm_support::CmDevice *CmDevicePtr = nullptr;

  /// Map SVM memory starting address to corresponding
  /// CmBufferSVM object. CmBufferSVM object is needed to release memory.
  std::unordered_map<void *, cm_support::CmBufferSVM *> Addr2CmBufferSVM;
};

struct _pi_queue : _pi_object {
  _pi_queue(pi_context ContextArg, cm_support::CmQueue *CmQueueArg)
      : Context{ContextArg}, CmQueuePtr{CmQueueArg} {}

  // Keeps the PI context to which this queue belongs.
  pi_context Context = nullptr;
  cm_support::CmQueue *CmQueuePtr = nullptr;
};

struct _pi_mem : _pi_object {
  _pi_mem() {}

  pi_context Context;

  char *MapHostPtr = nullptr;

  // Mutex for load/store accessing
  std::mutex mutexLock;

  // Surface index used by CM
  int SurfaceIndex;

  virtual ~_pi_mem() = default;

  _pi_mem_type getMemType() const { return MemType; };

protected:
  _pi_mem(pi_context ctxt, char *HostPtr, _pi_mem_type MemTypeArg,
          int SurfaceIdxArg)
      : Context{ctxt}, MapHostPtr{HostPtr},
        SurfaceIndex{SurfaceIdxArg}, MemType{MemTypeArg} {}

private:
  _pi_mem_type MemType;
};

struct _pi_buffer final : _pi_mem {
  // Buffer/Sub-buffer constructor
  _pi_buffer(pi_context ctxt, char *HostPtr, cm_support::CmBuffer *CmBufArg,
             int SurfaceIdxArg, size_t SizeArg)
      : _pi_mem(ctxt, HostPtr, PI_MEM_TYPE_BUFFER, SurfaceIdxArg),
        CmBufferPtr{CmBufArg}, Size{SizeArg} {}

  cm_support::CmBuffer *CmBufferPtr;
  size_t Size;
};

struct _pi_image final : _pi_mem {
  // Image constructor
  _pi_image(pi_context ctxt, char *HostPtr, cm_support::CmSurface2D *CmSurfArg,
            int SurfaceIdxArg, size_t WidthArg, size_t HeightArg, size_t BPPArg)
      : _pi_mem(ctxt, HostPtr, PI_MEM_TYPE_IMAGE2D, SurfaceIdxArg),
        CmSurfacePtr{CmSurfArg}, Width{WidthArg}, Height{HeightArg},
        BytesPerPixel{BPPArg} {}

  cm_support::CmSurface2D *CmSurfacePtr;
  size_t Width;
  size_t Height;
  size_t BytesPerPixel;
};

struct _pi_event : _pi_object {
  _pi_event() {}

  cm_support::CmEvent *CmEventPtr = nullptr;
  cm_support::CmQueue *OwnerQueue = nullptr;
  pi_context Context = nullptr;
  bool IsDummyEvent = false;
};

struct _pi_program : _pi_object {
  _pi_program() {}

  // Keep the context of the program.
  pi_context Context;
};

struct _pi_kernel : _pi_object {
  _pi_kernel() {}
};
