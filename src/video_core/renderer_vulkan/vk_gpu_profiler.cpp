// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project (fifa-hybrid fork, VladZ97)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fmt/format.h>
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

bool GpuProfiler::Enabled() {
    static const bool enabled = std::getenv("SHAD_GPU_PROFILE") != nullptr;
    return enabled;
}

GpuProfiler::GpuProfiler(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const auto props = instance.GetPhysicalDevice().getProperties();
    const auto families = instance.GetPhysicalDevice().getQueueFamilyProperties();
    const u32 fam = instance.GetGraphicsQueueFamilyIndex();
    const u32 valid_bits = fam < families.size() ? families[fam].timestampValidBits : 0;
    period_ns = props.limits.timestampPeriod;
    supported = valid_bits > 0 && props.limits.timestampComputeAndGraphics;
    LOG_INFO(Render_Vulkan, "GPU profiler: timestamps supported={} validBits={} period={}ns",
             supported, valid_bits, period_ns);
    if (!supported) {
        return;
    }
    // Metal counter sample buffers are small: try decreasing pool sizes.
    for (const u32 per_slice : {256u, 128u, 64u, 32u}) {
        kQueriesPerSlice = per_slice;
        const vk::QueryPoolCreateInfo info = {
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = kSlices * kQueriesPerSlice,
        };
        auto [result, p] = instance.GetDevice().createQueryPoolUnique(info);
        if (result == vk::Result::eSuccess) {
            pool = std::move(p);
            break;
        }
        LOG_WARNING(Render_Vulkan, "GPU profiler: query pool of {} failed ({})",
                    kSlices * kQueriesPerSlice, vk::to_string(result));
    }
    if (!pool) {
        supported = false;
        return;
    }
    LOG_INFO(Render_Vulkan, "GPU profiler: query pool {} x {} queries", kSlices, kQueriesPerSlice);
    slice_busy = std::vector<std::atomic<bool>>(kSlices);
    window_start = std::chrono::steady_clock::now();
    start_time = window_start;
}

GpuProfiler::~GpuProfiler() = default;

void GpuProfiler::OnNewCommandBuffer(vk::CommandBuffer cmdbuf) {
    if (!supported) {
        return;
    }
    current = Slice{};
    // pick a slice whose previous results were already read back; a slice still in flight
    // must not be reset (that lost the device on Metal). If all are busy, skip this buffer.
    u32 tries = 0;
    while (tries < kSlices && slice_busy[slice_index].load()) {
        slice_index = (slice_index + 1) % kSlices;
        ++tries;
    }
    if (tries == kSlices) {
        slice_active = false;
        segment_open = false;
        return;
    }
    current.index = slice_index;
    current.base = slice_index * kQueriesPerSlice;
    slice_index = (slice_index + 1) % kSlices;
    cmdbuf.resetQueryPool(*pool, current.base, kQueriesPerSlice);
    slice_active = true;
    segment_open = false;
}

void GpuProfiler::BeginSegment(vk::CommandBuffer cmdbuf, std::string label) {
    if (!supported || !slice_active) {
        return;
    }
    if (segment_open) {
        EndSegment(cmdbuf);
    }
    if (current.next_query + 2 > kQueriesPerSlice) {
        return; // slice full: drop the rest of this command buffer
    }
    Segment seg{};
    seg.label = std::move(label);
    seg.first_query = current.base + current.next_query;
    current.next_query += 2;
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, *pool, seg.first_query);
    current.segments.push_back(std::move(seg));
    segment_open = true;
}

void GpuProfiler::EndSegment(vk::CommandBuffer cmdbuf) {
    if (!supported || !segment_open) {
        return;
    }
    const auto& seg = current.segments.back();
    cmdbuf.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, *pool, seg.first_query + 1);
    segment_open = false;
}

void GpuProfiler::NoteDraw(std::span<const size_t> stage_hashes) {
    if (!supported || !segment_open || current.segments.empty()) {
        return;
    }
    auto& seg = current.segments.back();
    seg.draws++;
    if (!seg.labeled) {
        std::string s = seg.label;
        for (u32 i = 0; i < stage_hashes.size(); ++i) {
            if (stage_hashes[i]) {
                s += fmt::format(" s{}:{:016x}", i, static_cast<u64>(stage_hashes[i]));
            }
        }
        seg.label = std::move(s);
        seg.labeled = true;
    }
}

void GpuProfiler::OnEndCommandBuffer(vk::CommandBuffer cmdbuf) {
    if (!supported || !slice_active) {
        return;
    }
    if (segment_open) {
        EndSegment(cmdbuf);
    }
    slice_active = false;
    if (current.segments.empty()) {
        return;
    }
    // Read the results once the GPU has finished this submission.
    Slice done = std::move(current);
    current = Slice{};
    slice_busy[done.index].store(true);
    scheduler.DeferOperation([this, done = std::move(done)] {
        struct Release {
            std::atomic<bool>& flag;
            ~Release() { flag.store(false); }
        } release{slice_busy[done.index]};
        std::vector<u64> values(done.next_query);
        const auto result = instance.GetDevice().getQueryPoolResults(
            *pool, done.base, done.next_query, values.size() * sizeof(u64), values.data(),
            sizeof(u64), vk::QueryResultFlagBits::e64 | vk::QueryResultFlagBits::eWait);
        if (result != vk::Result::eSuccess) {
            return;
        }
        std::scoped_lock lk{agg_mutex};
        {
            // span from the first segment's start to the last segment's end: the difference to
            // the sum of segments is the work between passes (copies, clears, uploads)
            const u64 f0 = values[done.segments.front().first_query - done.base];
            const u64 f1 = values[done.segments.back().first_query + 1 - done.base];
            if (f1 > f0) {
                window_total_ms += static_cast<double>(f1 - f0) * period_ns / 1e6;
            }
        }
        for (const auto& seg : done.segments) {
            const u64 t0 = values[seg.first_query - done.base];
            const u64 t1 = values[seg.first_query + 1 - done.base];
            if (t1 <= t0) {
                continue;
            }
            const double ms = static_cast<double>(t1 - t0) * period_ns / 1e6;
            auto& a = agg[seg.label];
            a.ms += ms;
            a.count++;
            a.max_draws = std::max(a.max_draws, seg.draws);
            window_ms += ms;
        }
        window_sessions++;
        Report();
    });
}

void GpuProfiler::Report() {
    // called with agg_mutex held
    const auto now = std::chrono::steady_clock::now();
    const double secs = std::chrono::duration<double>(now - window_start).count();
    if (secs < 3.0) {
        return;
    }
    std::vector<std::pair<std::string, Agg>> rows(agg.begin(), agg.end());
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    const double t = std::chrono::duration<double>(now - start_time).count();
    std::string out = fmt::format(
        "GPUPROF t={:.0f}s window {:.1f}s: span {:.1f} GPU ms/s, passes {:.1f} ms/s, "
        "between-passes {:.1f} ms/s, {} submits\n",
        t, secs, window_total_ms / secs, window_ms / secs, (window_total_ms - window_ms) / secs,
        window_sessions);
    const u32 n = std::min<u32>(rows.size(), 16);
    for (u32 i = 0; i < n; ++i) {
        const auto& [label, a] = rows[i];
        out += fmt::format("  {:7.1f} ms/s {:6.1f}/s {:4d}dr  {}\n", a.ms / secs,
                           a.count / secs, a.max_draws, label);
    }
    LOG_INFO(Render_Vulkan, "{}", out);
    agg.clear();
    window_ms = 0;
    window_total_ms = 0;
    window_sessions = 0;
    window_start = now;
}

} // namespace Vulkan
