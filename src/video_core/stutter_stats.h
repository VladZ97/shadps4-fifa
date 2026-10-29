// Local tool: frame-time spikes and what happened inside them (SHAD_STUTTER=1).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdlib>
#include "common/types.h"

namespace VideoCore {

struct StutterStats {
    static StutterStats& Get() {
        static StutterStats s;
        return s;
    }
    static bool Enabled() {
        static const bool enabled = std::getenv("SHAD_STUTTER") != nullptr;
        return enabled;
    }
    using Clock = std::chrono::steady_clock;
    static u64 NowUs() {
        return std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch())
            .count();
    }
    struct Scope {
        std::atomic<u64>& count;
        std::atomic<u64>& us;
        u64 t0;
        Scope(std::atomic<u64>& c, std::atomic<u64>& u) : count{c}, us{u}, t0{NowUs()} {}
        ~Scope() {
            ++count;
            us += NowUs() - t0;
        }
    };
    std::atomic<u64> pipelines{0}, pipeline_us{0};
    std::atomic<u64> modules{0}, module_us{0};
    std::atomic<u64> uploads{0}, upload_bytes{0}, upload_us{0};
    std::atomic<u64> gc_runs{0}, gc_us{0};
    std::atomic<u64> faults{0}, fault_us{0};
    std::atomic<u64> finishes{0}, finish_us{0};
    std::atomic<u64> file_opens{0}, file_open_us{0};

    /// Called at every flip; logs spikes and a periodic summary.
    void OnFlip();

private:
    u64 last_flip_us = 0;
    u64 window_start_us = 0;
    u32 frames = 0, slow25 = 0, slow40 = 0;
    u64 max_us = 0, sum_us = 0;
    struct Snap {
        u64 pipelines, pipeline_us, modules, module_us, uploads, upload_bytes, upload_us, gc_runs,
            gc_us, faults, fault_us, finishes, finish_us, file_opens, file_open_us;
    };
    Snap Take() const {
        return {pipelines, pipeline_us, modules, module_us, uploads, upload_bytes, upload_us,
                gc_runs, gc_us, faults, fault_us, finishes, finish_us, file_opens, file_open_us};
    }
    Snap at_last_flip{}, at_window_start{};
};

} // namespace VideoCore
