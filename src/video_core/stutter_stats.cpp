// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project (fifa-hybrid fork, VladZ97)
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>
#include "common/logging/log.h"
#include "video_core/stutter_stats.h"

namespace VideoCore {

void StutterStats::OnFlip() {
    const u64 now = NowUs();
    if (last_flip_us == 0) {
        last_flip_us = now;
        window_start_us = now;
        at_last_flip = Take();
        at_window_start = at_last_flip;
        return;
    }
    const u64 dt = now - last_flip_us;
    last_flip_us = now;
    const Snap s = Take();
    ++frames;
    sum_us += dt;
    max_us = std::max(max_us, dt);
    if (dt > 25000) {
        ++slow25;
    }
    if (dt > 40000) {
        ++slow40;
        const auto& a = at_last_flip;
        LOG_INFO(Render,
                 "STUTTER spike {:.1f} ms: pipelines {} ({:.1f} ms) modules {} ({:.1f} ms) uploads {} "
                 "({} KB, {:.1f} ms) gc {} ({:.1f} ms) faults {} ({:.1f} ms) finishes {} ({:.1f} ms) "
                 "opens {} ({:.1f} ms)",
                 dt / 1000.0, s.pipelines - a.pipelines, (s.pipeline_us - a.pipeline_us) / 1000.0,
                 s.modules - a.modules, (s.module_us - a.module_us) / 1000.0, s.uploads - a.uploads,
                 (s.upload_bytes - a.upload_bytes) / 1024, (s.upload_us - a.upload_us) / 1000.0,
                 s.gc_runs - a.gc_runs, (s.gc_us - a.gc_us) / 1000.0, s.faults - a.faults,
                 (s.fault_us - a.fault_us) / 1000.0, s.finishes - a.finishes,
                 (s.finish_us - a.finish_us) / 1000.0, s.file_opens - a.file_opens,
                 (s.file_open_us - a.file_open_us) / 1000.0);
    }
    at_last_flip = s;
    if (now - window_start_us >= 5000000) {
        const auto& a = at_window_start;
        const double secs = (now - window_start_us) / 1e6;
        LOG_INFO(Render,
                 "STUTTER window {:.0f}s: {} frames avg {:.1f} ms max {:.1f} ms >25ms {} >40ms {} | "
                 "pipelines {} ({:.0f} ms) modules {} ({:.0f} ms) uploads {} ({} MB, {:.0f} ms) gc {} "
                 "({:.0f} ms) faults {} ({:.0f} ms) finishes {} ({:.0f} ms) opens {} ({:.0f} ms)",
                 secs, frames, frames ? sum_us / 1000.0 / frames : 0.0, max_us / 1000.0, slow25,
                 slow40, s.pipelines - a.pipelines, (s.pipeline_us - a.pipeline_us) / 1000.0,
                 s.modules - a.modules, (s.module_us - a.module_us) / 1000.0, s.uploads - a.uploads,
                 (s.upload_bytes - a.upload_bytes) >> 20, (s.upload_us - a.upload_us) / 1000.0,
                 s.gc_runs - a.gc_runs, (s.gc_us - a.gc_us) / 1000.0, s.faults - a.faults,
                 (s.fault_us - a.fault_us) / 1000.0, s.finishes - a.finishes,
                 (s.finish_us - a.finish_us) / 1000.0, s.file_opens - a.file_opens,
                 (s.file_open_us - a.file_open_us) / 1000.0);
        window_start_us = now;
        at_window_start = s;
        frames = slow25 = slow40 = 0;
        max_us = sum_us = 0;
    }
}

} // namespace VideoCore
