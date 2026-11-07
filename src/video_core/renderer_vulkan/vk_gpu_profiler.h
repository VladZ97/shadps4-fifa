// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project (fifa-hybrid fork, VladZ97)
// SPDX-License-Identifier: GPL-2.0-or-later

// Local tool: per-render-pass / per-dispatch GPU timing with timestamp queries.
// Enabled with SHAD_GPU_PROFILE=1; prints the heaviest segments every few seconds.
#pragma once

#include <array>
#include <atomic>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;

class GpuProfiler {
public:
    static bool Enabled();

    GpuProfiler(const Instance& instance, Scheduler& scheduler);
    ~GpuProfiler();

    /// A new primary command buffer started (BeginSession): take a query slice and reset it.
    void OnNewCommandBuffer(vk::CommandBuffer cmdbuf);
    /// The command buffer is about to be ended (EndSession): close segments, queue the readback.
    void OnEndCommandBuffer(vk::CommandBuffer cmdbuf);

    void BeginSegment(vk::CommandBuffer cmdbuf, std::string label);
    void EndSegment(vk::CommandBuffer cmdbuf);
    /// First draw of a render pass labels it with its shader hashes; later draws are counted.
    void NoteDraw(std::span<const size_t> stage_hashes);

private:
    struct Segment {
        std::string label;
        u32 draws = 0;
        u32 first_query = 0;
        bool labeled = false;
    };
    struct Slice {
        u32 base = 0;
        u32 index = 0;
        std::vector<Segment> segments;
        u32 next_query = 0;
        u32 frame_query = 0; // [frame_query, frame_query+1] = whole command buffer
    };
    u32 kSlices = 8;
    u32 kQueriesPerSlice = 256;

    void Report();

    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    bool supported = false;
    double period_ns = 1.0;
    u32 slice_index = 0;
    std::vector<std::atomic<bool>> slice_busy; ///< results not read yet: never reset such a slice
    Slice current;
    bool slice_active = false;
    bool segment_open = false;

    struct Agg {
        double ms = 0;
        u32 count = 0;
        u32 max_draws = 0;
    };
    std::mutex agg_mutex;
    std::unordered_map<std::string, Agg> agg;
    double window_ms = 0;
    double window_total_ms = 0;
    std::chrono::steady_clock::time_point start_time;
    u32 window_sessions = 0;
    std::chrono::steady_clock::time_point window_start;
};

} // namespace Vulkan
