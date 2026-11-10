// SPDX-License-Identifier: GPL-2.0-or-later
// fifa-hybrid: see bbgpu_bridge.h. The runtime_* functions below are the contract the GPU
// library links against (gpu/CMakeLists.txt BB_HOST_RUNTIME_SYMBOLS in bloodborne_mac_port);
// they resolve against this executable at load time.

#include <signal.h>
#include "core/bbgpu_bridge.h"

#ifdef ENABLE_BBGPU

#include <atomic>
#include <thread>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <vector>
#include <setjmp.h>
#include <sys/mman.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#endif
#include "bbgpu.h"
extern "C" {
#include "core/bbgpu_split/gpu_process.h"
}
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/rdtsc.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/time.h"
#include "core/memory.h"
#include <SDL3/SDL.h>
#include "common/thread.h"
#include "sdl_window.h"

extern "C" {
void bbgpu_memory_register(uint64_t address, uint64_t size);
void bbgpu_memory_map(uint64_t address, uint64_t size);
void bbgpu_memory_unmap(uint64_t address, uint64_t size);
void bbgpu_memory_invalidate(uint64_t address, uint64_t size);
void bbgpu_memory_protect(uint64_t address, uint64_t size);
}

#define BB_EXPORT extern "C" __attribute__((visibility("default"), used))

namespace {

struct Region {
    VAddr end;
    u32 prot;
    s32 type;
};
std::mutex regions_mutex;
std::map<VAddr, Region> regions; // mapped guest ranges, by start

std::atomic<bool> initialized{false};
const auto process_start = std::chrono::steady_clock::now();
std::string g_title, g_serial, g_user_dir;

void RemoveLocked(VAddr addr, u64 size) {
    const VAddr end = addr + size;
    auto it = regions.upper_bound(addr);
    if (it != regions.begin()) {
        --it;
        if (it->second.end <= addr) {
            ++it;
        }
    }
    while (it != regions.end() && it->first < end) {
        const VAddr r_start = it->first;
        const Region r = it->second;
        it = regions.erase(it);
        if (r_start < addr) {
            regions.emplace(r_start, Region{addr, r.prot, r.type});
        }
        if (r.end > end) {
            it = regions.emplace(end, Region{r.end, r.prot, r.type}).first;
            ++it;
        }
    }
}

std::map<VAddr, Region>::const_iterator FindLocked(VAddr addr) {
    auto it = regions.upper_bound(addr);
    if (it == regions.begin()) {
        return regions.end();
    }
    --it;
    return addr < it->second.end ? it : regions.end();
}

u64 Nanoseconds() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                process_start)
        .count();
}

} // namespace

namespace Core {
void BbSplitMirrorProt(VAddr addr, u64 size, u32 prot); // address_space.cpp
}

namespace Core::BbGpu {

bool Enabled() {
    static const bool enabled = [] {
        const char* v = std::getenv("SHAD_BBGPU");
        return v && v[0] == '1';
    }();
    return enabled;
}

namespace {
// Polls the GPU host's keyboard state (shared block, SDL scancode-indexed) and turns changes
// into key events for the emulator's window, so the input handler sees them as usual.
void StartSplitKeyboard(SDL_WindowID window_id) {
    std::thread([window_id] {
        Common::SetCurrentThreadName("bb:SplitKeys");
        std::array<bool, 512> previous{};
        for (;;) {
            if (const bool* keys = bbgpu_split_keyboard()) {
                for (int sc = 0; sc < int(previous.size()); ++sc) {
                    if (keys[sc] == previous[sc]) {
                        continue;
                    }
                    previous[sc] = keys[sc];
                    SDL_Event event{};
                    event.type = keys[sc] ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                    event.key.windowID = window_id;
                    event.key.scancode = static_cast<SDL_Scancode>(sc);
                    event.key.key = SDL_GetKeyFromScancode(event.key.scancode, SDL_KMOD_NONE, false);
                    event.key.down = keys[sc];
                    event.key.timestamp = SDL_GetTicksNS();
                    SDL_PushEvent(&event);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // fifa-hybrid: 1 ms keyboard poll (was 4)
        }
    }).detach();
}
} // namespace

bool SplitEnabled() {
    static const bool split = [] {
        const char* v = std::getenv("SHAD_BBGPU_SPLIT");
        return v && v[0] == '1';
    }();
    return split;
}

bool Init(Frontend::WindowSDL& window, const std::string& serial, const std::string& title,
          u32 sdk_version, u32 psf_attributes) {
    const auto info = window.GetWindowInfo();
    if (!info.render_surface) {
        LOG_CRITICAL(Render, "bbgpu: the window has no Metal layer");
        return false;
    }
    g_title = title.empty() ? "shadPS4 (bbgpu)" : title;
    g_serial = serial.empty() ? "UNKNOWN" : serial;
    g_user_dir = (Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "bbgpu").string();
    std::filesystem::create_directories(g_user_dir);

    bbgpu_register_kernel();
    BbGpuConfig config{};
    config.title = g_title.c_str();
    config.serial = g_serial.c_str();
    config.user_dir = g_user_dir.c_str();
    config.sdk_version = sdk_version;
    config.psf_attributes = psf_attributes;
    config.width = window.GetWidth();
    config.height = window.GetHeight();
    config.metal_layer = info.render_surface;
    config.layer_scale = info.render_surface_scale;
    if (SplitEnabled()) {
        // The renderer runs in the native arm64 GPU process, in its own window.
        config.metal_layer = nullptr;
        config.layer_scale = 0.0f;
        if (runtime_memory_create_pool() != 0) {
            LOG_CRITICAL(Render, "bbgpu: no shared memory entry for the guest memory");
            return false;
        }
        gpu_process_start(); // exits the emulator with a "Crash:" line if the host fails
        if (bbgpu_init_remote(&config, gpu_process_shared()) != 0) {
            LOG_CRITICAL(Render, "bbgpu: the GPU process could not start the renderer");
            return false;
        }
        gpu_process_rendering();
        // The picture is in the GPU host's window: hide ours (it would take focus) and take the
        // keyboard from the host window instead, as SDL key events on this window.
        SDL_Window* const own = window.GetSDLWindow();
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_HideWindow(own);
        StartSplitKeyboard(SDL_GetWindowID(own));
        initialized = true;
        LOG_INFO(Render, "bbgpu: rendering in the native GPU process, {} HLE symbols",
                 bbgpu_symbol_count());
        return true;
    }
    if (bbgpu_init(&config) != 0) {
        LOG_CRITICAL(Render, "bbgpu: bbgpu_init failed");
        return false;
    }
    initialized = true;
    LOG_INFO(Render, "bbgpu: renderer started on the window's Metal layer, {} HLE symbols, user dir {}",
             bbgpu_symbol_count(), g_user_dir);
    return true;
}

u64 Resolve(const std::string& scoped_nid) {
    if (!initialized) {
        return 0;
    }
    return bbgpu_resolve(scoped_nid.c_str());
}

void OnRegister(VAddr addr, u64 size) {
    if (initialized) {
        bbgpu_memory_register(addr, size);
    }
}

void OnMap(VAddr addr, u64 size, u32 prot, s32 type) {
    if (SplitEnabled()) {
        BbSplitMirrorProt(addr, size, prot);
    }
    {
        std::scoped_lock lock{regions_mutex};
        RemoveLocked(addr, size);
        regions.emplace(addr, Region{addr + size, prot, type});
    }
    if (initialized) {
        bbgpu_memory_map(addr, size);
    }
}

void OnUnmap(VAddr addr, u64 size) {
    if (initialized) {
        bbgpu_memory_unmap(addr, size);
    }
    std::scoped_lock lock{regions_mutex};
    RemoveLocked(addr, size);
}

void OnInvalidate(VAddr addr, u64 size) {
    if (initialized) {
        bbgpu_memory_invalidate(addr, size);
    }
}

void OnProtect(VAddr addr, u64 size, u32 prot) {
    if (SplitEnabled()) {
        BbSplitMirrorProt(addr, size, prot);
    }
    {
        std::scoped_lock lock{regions_mutex};
        // Re-tag the mapped parts of [addr, addr+size) with the new protection.
        std::vector<std::pair<VAddr, Region>> pieces;
        auto it = regions.upper_bound(addr);
        if (it != regions.begin()) {
            --it;
        }
        for (; it != regions.end() && it->first < addr + size; ++it) {
            const VAddr s = std::max<VAddr>(it->first, addr);
            const VAddr e = std::min<VAddr>(it->second.end, addr + size);
            if (s < e) {
                pieces.emplace_back(s, Region{e, prot, it->second.type});
            }
        }
        for (const auto& [s, r] : pieces) {
            RemoveLocked(s, r.end - s);
            regions.emplace(s, r);
        }
    }
    if (initialized) {
        bbgpu_memory_protect(addr, size);
    }
}

} // namespace Core::BbGpu

// ---- the runtime contract -------------------------------------------------------------------

BB_EXPORT __thread sigjmp_buf* runtime_fault_recover = nullptr;
// BB_DISABLE_OPT=<hex mask of BbToggle bits> switches library optimizations/heuristics off.
BB_EXPORT uint64_t runtime_disabled_optimizations =
    std::getenv("BB_DISABLE_OPT") ? std::strtoull(std::getenv("BB_DISABLE_OPT"), nullptr, 16) : 0;

namespace Core::BbGpu {
bool HandleFault(void* ucontext, void* address) {
    // A renderer thread that asked for recovery (BbGuest::Read) must not enter the page
    // manager: a read of a page the precise readback mode protects would wait for a download
    // that the faulting thread itself performs. It discards the read and retries later.
    if (sigjmp_buf* recover = runtime_fault_recover) {
        runtime_fault_recover = nullptr;
        // The jump leaves the handler without restoring the mask (sigsetjmp(buf, 0)), so the
        // fault signals would stay blocked and the thread's next fault would hang it forever.
        sigset_t faults;
        sigemptyset(&faults);
        sigaddset(&faults, SIGSEGV);
        sigaddset(&faults, SIGBUS);
        pthread_sigmask(SIG_UNBLOCK, &faults, nullptr);
        siglongjmp(*recover, 1);
    }
    return initialized && bbgpu_handle_fault(ucontext, address);
}
} // namespace Core::BbGpu

BB_EXPORT int runtime_memory_region(uintptr_t address, uintptr_t* start, uintptr_t* end,
                                    int* mapped) {
    std::scoped_lock lock{regions_mutex};
    auto it = FindLocked(address);
    if (it != regions.end()) {
        if (start) {
            *start = it->first;
        }
        if (end) {
            *end = it->second.end;
        }
        if (mapped) {
            *mapped = 1;
        }
        return 1;
    }
    // A gap: from the previous mapping's end to the next mapping's start.
    auto next = regions.upper_bound(address);
    if (next == regions.end()) {
        return 0; // past the last mapping
    }
    if (start) {
        *start = next == regions.begin() ? 0 : std::prev(next)->second.end;
    }
    if (end) {
        *end = next->first;
    }
    if (mapped) {
        *mapped = 0;
    }
    return 1;
}

BB_EXPORT int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end) {
    std::scoped_lock lock{regions_mutex};
    auto it = FindLocked(address);
    if (it == regions.end()) {
        return 0;
    }
    if (prot) {
        *prot = int(it->second.prot);
    }
    if (type) {
        *type = it->second.type;
    }
    if (end) {
        *end = it->second.end;
    }
    return 1;
}

BB_EXPORT uint64_t runtime_memory_clamp(uintptr_t address, uint64_t size) {
    std::scoped_lock lock{regions_mutex};
    uint64_t mapped = 0;
    VAddr cursor = address;
    while (mapped < size) {
        auto it = FindLocked(cursor);
        if (it == regions.end()) {
            break;
        }
        const u64 n = std::min<u64>(size - mapped, it->second.end - cursor);
        mapped += n;
        cursor += n;
    }
    return mapped;
}

BB_EXPORT int runtime_memory_is_mapped(uintptr_t address, uint64_t size) {
    return runtime_memory_clamp(address, size) == size;
}

BB_EXPORT int runtime_memory_read_backing(uintptr_t address, void* out, uint64_t size) {
    auto* memory = Core::Memory::Instance();
    const void* backing = memory->TryGetBacking(reinterpret_cast<const void*>(address), size);
    if (!backing) {
        return 0;
    }
    std::memcpy(out, backing, size);
    return 1;
}

BB_EXPORT int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size) {
    auto* memory = Core::Memory::Instance();
    return memory->TryWriteBacking(reinterpret_cast<void*>(address), data, size) ? 1 : 0;
}

BB_EXPORT void runtime_memory_gpu_protect(uintptr_t address, uint64_t size, int read, int write) {
    constexpr u64 Page = 4096;
    {
        // Diagnostics: how the renderer drives page protection (SHAD_BBGPU_PROT_LOG=1 logs each).
        static std::atomic<u64> calls{0}, none_bytes{0}, ro_bytes{0}, rw_bytes{0}, unmirrored{0};
        static const bool log_each = std::getenv("SHAD_BBGPU_PROT_LOG") != nullptr;
        const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
        (read ? (write ? rw_bytes : ro_bytes) : none_bytes).fetch_add(size, std::memory_order_relaxed);
        {
            std::scoped_lock lock{regions_mutex};
            if (FindLocked(address) == regions.end()) {
                unmirrored.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (log_each || (n % 5000) == 1) {
            LOG_INFO(Render, "bbgpu protect #{}: {:#x}+{:#x} read={} write={} | totals none={} MiB ro={} MiB rw={} MiB, calls outside the mirror={}",
                     n, address, size, read, write, none_bytes.load() >> 20, ro_bytes.load() >> 20,
                     rw_bytes.load() >> 20, unmirrored.load());
        }
    }
    const VAddr begin = address & ~(Page - 1);
    const VAddr end = (address + size + Page - 1) & ~(Page - 1);
    std::scoped_lock lock{regions_mutex};
    VAddr cursor = begin;
    while (cursor < end) {
        auto it = FindLocked(cursor);
        if (it == regions.end()) {
            auto next = regions.upper_bound(cursor);
            if (next == regions.end() || next->first >= end) {
                break;
            }
            cursor = next->first;
            continue;
        }
        const VAddr r_end = std::min<VAddr>(it->second.end, end);
        const u32 guest = it->second.prot;
        int prot = 0;
        if (read && (guest & 3)) {
            prot |= PROT_READ;
        }
        if (write && (guest & 2)) {
            prot |= PROT_WRITE;
        }
        if (guest & 4) {
            prot |= PROT_EXEC | PROT_READ;
        }
        if (mprotect(reinterpret_cast<void*>(cursor), r_end - cursor, prot) != 0) {
            LOG_WARNING(Render, "bbgpu: mprotect({:#x}, {:#x}, {}) failed: {}", cursor,
                        r_end - cursor, prot, strerror(errno));
        }
        cursor = r_end;
    }
}

BB_EXPORT void runtime_memory_set_gpu_hooks(void (*map)(uintptr_t, uint64_t),
                                            void (*unmap)(uintptr_t, uint64_t),
                                            void (*invalidate)(uintptr_t, uint64_t)) {
    // The bridge drives the renderer's tracking through bbgpu_memory_* instead (same calls
    // MemoryManager makes on its own rasterizer); nothing to replay, the library starts before
    // the game maps memory.
    (void)map;
    (void)unmap;
    (void)invalidate;
}

BB_EXPORT uint64_t runtime_process_time_us(void) {
    return Nanoseconds() / 1000;
}
BB_EXPORT uint64_t runtime_process_time_counter(void) {
    return Nanoseconds();
}
BB_EXPORT uint64_t runtime_tsc_frequency(void) {
    // Before the kernel library's clock exists (the split renderer starts before the game), the
    // same estimate the native clock is built from.
    static const u64 frequency = Common::EstimateRDTSCFrequency();
    return frequency;
}
BB_EXPORT int32_t* runtime_errno(void) {
    return Libraries::Kernel::__Error();
}

BB_EXPORT int runtime_file_translate(const char* guest, char* out, size_t out_size) {
    std::string path = guest ? guest : "";
    if (path.find("..") != std::string::npos) {
        return ENOENT;
    }
    if (path.empty() || path[0] != '/') {
        path = "/app0/" + path;
    }
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const auto host = mnt->GetHostPath(path);
    if (host.empty()) {
        return ENOENT;
    }
    const std::string s = host.string();
    if (s.size() + 1 > out_size) {
        return ENAMETOOLONG;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return 0;
}

BB_EXPORT void runtime_thread_attach_host(const char* name) {
    (void)name;
}
BB_EXPORT void runtime_thread_set_blocked(const char* what, uint64_t resource) {
    (void)what;
    (void)resource;
}
BB_EXPORT void runtime_thread_clear_blocked(void) {}
#ifdef __APPLE__
// Local change (0.5.1-f): CPU time of the game's threads for the renderer's frame spike log
// (SHAD_GAME_CPU=1; one thread_info per thread at every flip, so off by default).
namespace {
bool GameCpuStats() {
    static const bool on = [] {
        const char* v = std::getenv("SHAD_GAME_CPU");
        return v && v[0] == '1';
    }();
    return on;
}
u64 PortCpuNs(mach_port_t port) {
    thread_basic_info_data_t info;
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count) !=
        KERN_SUCCESS) {
        return 0;
    }
    return (u64(info.user_time.seconds) + u64(info.system_time.seconds)) * 1000000000ull +
           (u64(info.user_time.microseconds) + u64(info.system_time.microseconds)) * 1000ull;
}
void PortName(mach_port_t port, char name[32]) {
    name[0] = '\0';
    if (const pthread_t thread = pthread_from_mach_thread_np(port)) {
        pthread_getname_np(thread, name, 32);
    }
}
std::atomic<mach_port_t> g_game_main_port{MACH_PORT_NULL};
} // namespace
#endif
BB_EXPORT uint64_t runtime_thread_cpu_ns(int which) {
#ifdef __APPLE__
    if (!GameCpuStats()) {
        return 0;
    }
    if (which) {
        return PortCpuNs(pthread_mach_thread_np(pthread_self()));
    }
    const mach_port_t main_port = g_game_main_port.load(std::memory_order_relaxed);
    return main_port != MACH_PORT_NULL ? PortCpuNs(main_port) : 0;
#else
    (void)which;
    return 0;
#endif
}
BB_EXPORT void runtime_game_cpu_since_last(uint64_t* total, uint64_t* busiest, char name[32]) {
    *total = 0;
    *busiest = 0;
    name[0] = '\0';
#ifdef __APPLE__
    if (!GameCpuStats()) {
        return;
    }
    static std::mutex mutex;
    static std::map<mach_port_t, u64> last;
    std::scoped_lock lock{mutex};
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) {
        return;
    }
    std::map<mach_port_t, u64> now;
    mach_port_t busiest_port = MACH_PORT_NULL;
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        const mach_port_t port = threads[i];
        const u64 ns = PortCpuNs(port);
        now[port] = ns;
        const auto it = last.find(port);
        const u64 before = it != last.end() ? it->second : ns;
        const u64 delta = ns >= before ? ns - before : 0;
        *total += delta;
        if (delta > *busiest) {
            *busiest = delta;
            busiest_port = port;
        }
        if (g_game_main_port.load(std::memory_order_relaxed) == MACH_PORT_NULL) {
            char thread_name[32];
            PortName(port, thread_name);
            if (!std::strcmp(thread_name, "Game:Main")) {
                g_game_main_port.store(port, std::memory_order_relaxed);
            }
        }
    }
    if (busiest_port != MACH_PORT_NULL) {
        PortName(busiest_port, name);
    }
    for (mach_msg_type_number_t i = 0; i < count; ++i) {
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), reinterpret_cast<vm_address_t>(threads),
                  count * sizeof(thread_act_t));
    last.swap(now);
#endif
}
BB_EXPORT void runtime_sampler_flip(uint64_t main_cpu_ns) {
    (void)main_cpu_ns;
}
BB_EXPORT const char* runtime_context_lock_other(void) {
    return nullptr;
}
BB_EXPORT void runtime_restart(void) {
    LOG_WARNING(Render, "bbgpu: restart requested by the renderer (ignored)");
}

#else // !ENABLE_BBGPU

namespace Core::BbGpu {
bool Enabled() {
    return false;
}
bool Init(Frontend::WindowSDL&, const std::string&, const std::string&, u32, u32) {
    return false;
}
u64 Resolve(const std::string&) {
    return 0;
}
void OnRegister(VAddr, u64) {}
void OnMap(VAddr, u64, u32, s32) {}
void OnUnmap(VAddr, u64) {}
void OnInvalidate(VAddr, u64) {}
bool HandleFault(void*, void*) {
    return false;
}
} // namespace Core::BbGpu

#endif
