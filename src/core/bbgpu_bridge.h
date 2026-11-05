// SPDX-License-Identifier: GPL-2.0-or-later
// fifa-hybrid: bridge to the bloodborne_mac_port GPU library (libbbgpu.dylib): shadPS4 stays the
// runtime (kernel, memory, libraries) and the library renders the Gnm command streams through
// its native Metal renderer. Enabled at runtime with SHAD_BBGPU=1 (compile-time ENABLE_BBGPU).
#pragma once

#include <string>
#include "common/types.h"

namespace Frontend {
class WindowSDL;
}

namespace Core::BbGpu {

/// True when the build carries the bridge and SHAD_BBGPU=1 is set.
bool Enabled();

/// Starts the library's renderer on the given window's Metal layer and registers its
/// GnmDriver/VideoOut/event-queue symbols. Call before InitHLELibs. False on failure.
bool Init(Frontend::WindowSDL& window, const std::string& serial, const std::string& title,
          u32 sdk_version, u32 psf_attributes);

/// Address of an HLE function the library provides for "NID#library#module", or 0.
u64 Resolve(const std::string& scoped_nid);

/// Memory tracking, mirroring what MemoryManager tells its own rasterizer. The caller holds the
/// memory lock; the bridge records the range so later region queries from the library's threads
/// never take that lock.
void OnRegister(VAddr addr, u64 size);
void OnMap(VAddr addr, u64 size, u32 prot, s32 type);
void OnUnmap(VAddr addr, u64 size);
void OnInvalidate(VAddr addr, u64 size);
void OnProtect(VAddr addr, u64 size, u32 prot);

/// Access-violation path: true when the library handled the fault (the access is retried) or
/// when the faulting thread asked for recovery (does not return then).
bool HandleFault(void* ucontext, void* address);

} // namespace Core::BbGpu
