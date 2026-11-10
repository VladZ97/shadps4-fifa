# Changes to upstream shadPS4 in this fork (branch metal-hybrid)

GPLv2 section 2(a): the files below were modified or added for the FIFA 16 (CUSA02138) Metal hybrid
port. Dates are the commit dates; the full history is in git. Upstream: https://github.com/shadps4-emu/shadPS4

| status | file | last changed |
|---|---|---|
| M | `CMakeLists.txt` | 2026-11-10 |
| M | `src/common/path_util.cpp` | 2026-11-06 |
| M | `src/core/address_space.cpp` | 2026-11-03 |
| A | `src/core/bbgpu_bridge.cpp` | 2026-11-10 |
| A | `src/core/bbgpu_bridge.h` | 2026-11-06 |
| A | `src/core/bbgpu_split/gpu_process.c` | 2026-11-06 |
| A | `src/core/bbgpu_split/gpu_process.h` | 2026-11-06 |
| M | `src/core/emulator_settings.cpp` | 2026-11-06 |
| M | `src/core/file_sys/backends/host_fs.cpp` | 2026-10-28 |
| A | `src/core/file_sys/dir_index.cpp` | 2026-10-28 |
| A | `src/core/file_sys/dir_index.h` | 2026-10-28 |
| M | `src/core/file_sys/fs.cpp` | 2026-11-07 |
| M | `src/core/libraries/gnmdriver/gnmdriver.cpp` | 2026-10-28 |
| M | `src/core/libraries/kernel/file_system.cpp` | 2026-11-06 |
| M | `src/core/libraries/kernel/kernel.cpp` | 2026-11-03 |
| M | `src/core/libraries/kernel/process.cpp` | 2026-11-06 |
| M | `src/core/libraries/libs.cpp` | 2026-11-03 |
| M | `src/core/libraries/videoout/driver.cpp` | 2026-10-29 |
| M | `src/core/libraries/videoout/video_out.cpp` | 2026-10-28 |
| M | `src/core/linker.cpp` | 2026-11-10 |
| M | `src/core/linker.h` | 2026-11-06 |
| M | `src/core/memory.cpp` | 2026-11-10 |
| M | `src/core/memory.h` | 2026-11-06 |
| M | `src/core/module.cpp` | 2026-11-06 |
| M | `src/core/module.h` | 2026-11-06 |
| M | `src/core/signals.cpp` | 2026-11-06 |
| M | `src/core/user_manager.cpp` | 2026-11-07 |
| M | `src/emulator.cpp` | 2026-11-06 |
| M | `src/input/controller.cpp` | 2026-11-06 |
| M | `src/sdl_window.cpp` | 2026-11-06 |
| M | `src/shader_recompiler/frontend/fetch_shader.cpp` | 2026-10-28 |
| M | `src/video_core/amdgpu/liverpool.cpp` | 2026-10-28 |
| M | `src/video_core/amdgpu/liverpool.h` | 2026-10-28 |
| M | `src/video_core/amdgpu/pm4_cmds.h` | 2026-10-28 |
| M | `src/video_core/amdgpu/regs_shader.h` | 2026-10-28 |
| M | `src/video_core/buffer_cache/buffer_cache.cpp` | 2026-10-29 |
| M | `src/video_core/buffer_cache/buffer_cache.h` | 2026-10-29 |
| M | `src/video_core/cache_storage.cpp` | 2026-10-28 |
| M | `src/video_core/cache_storage.h` | 2026-10-28 |
| M | `src/video_core/page_manager.cpp` | 2026-10-28 |
| M | `src/video_core/page_manager.h` | 2026-10-28 |
| A | `src/video_core/renderer_vulkan/vk_gpu_profiler.cpp` | 2026-10-29 |
| A | `src/video_core/renderer_vulkan/vk_gpu_profiler.h` | 2026-10-29 |
| M | `src/video_core/renderer_vulkan/vk_pipeline_cache.cpp` | 2026-10-29 |
| M | `src/video_core/renderer_vulkan/vk_pipeline_serialization.cpp` | 2026-10-28 |
| M | `src/video_core/renderer_vulkan/vk_rasterizer.cpp` | 2026-10-29 |
| M | `src/video_core/renderer_vulkan/vk_rasterizer.h` | 2026-10-29 |
| M | `src/video_core/renderer_vulkan/vk_scheduler.cpp` | 2026-10-29 |
| M | `src/video_core/renderer_vulkan/vk_scheduler.h` | 2026-10-29 |
| A | `src/video_core/stutter_stats.cpp` | 2026-10-29 |
| A | `src/video_core/stutter_stats.h` | 2026-10-29 |
| M | `src/video_core/texture_cache/image_info.cpp` | 2026-10-28 |
| M | `src/video_core/texture_cache/image_view.cpp` | 2026-10-28 |
| M | `src/video_core/texture_cache/texture_cache.cpp` | 2026-10-29 |
| M | `src/video_core/texture_cache/texture_cache.h` | 2026-10-29 |
| M | `src/common/thread.cpp` | 2026-11-10 |
| M | `src/common/thread.h` | 2026-11-10 |
| M | `src/core/libraries/kernel/equeue.cpp` | 2026-11-10 |
| M | `src/core/libraries/kernel/threads/mutex.cpp` | 2026-11-10 |
| M | `src/core/libraries/kernel/threads/pthread.cpp` | 2026-11-10 |
| M | `src/core/thread.cpp` | 2026-11-10 |
| M | `src/main.cpp` | 2026-11-10 |
| M | `src/core/cpu_patches.cpp` | 2026-11-10 |
| M | `src/core/loader/symbols_resolver.cpp` | 2026-11-10 |
| M | `src/core/loader/symbols_resolver.h` | 2026-11-10 |
| M | `externals/CMakeLists.txt` | 2026-11-10 |
| M | `src/core/libraries/ajm/ajm_aac.cpp` | 2026-11-10 |
| M | `src/core/libraries/ajm/ajm_context.cpp` | 2026-11-10 |
| M | `src/core/libraries/ajm/ajm_instance.cpp` | 2026-11-10 |
| M | `src/core/net/p2p_crypto.cpp` | 2026-11-10 |

## Libraries left out

`ENABLE_GPL_INCOMPATIBLE_LIBS` (default OFF in this fork) controls two libraries of upstream shadPS4
whose licences are not compatible with the GPL: the Fraunhofer FDK AAC decoder and LibreSSL (which
carries the OpenSSL and SSLeay licences). The binaries of this fork are redistributed, so they are
built without both: creating an AAC decoder instance returns `ORBIS_AJM_ERROR_CODEC_NOT_SUPPORTED`
(FIFA 16 registers the codec but never creates an instance), libSceHttp HTTPS falls back to upstream's
mock path, and the P2P packet protection (AES-128-CFB, HMAC-SHA1) uses the system's CommonCrypto.
Set the option to ON for a private build with upstream's behaviour.
