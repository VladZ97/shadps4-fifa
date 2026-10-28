// SPDX-License-Identifier: GPL-2.0-or-later
// Local change (FIFA 16 on macOS): per-directory name index so case-insensitive lookups of missing
// files cost one hash lookup instead of a stat plus a full directory listing per probe. Games that
// probe dozens of candidate asset names per object spent most of their loading time in those scans.
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace Core::FileSys {

class DirIndex {
public:
    static DirIndex& Instance();

    /// Actual on-disk name of `name` inside `dir` (case-insensitive), or nullopt if absent.
    std::optional<std::filesystem::path> Lookup(const std::filesystem::path& dir,
                                                std::string_view name);
    /// Drop the cached listing of `dir` (call after creating/removing/renaming entries in it).
    void Invalidate(const std::filesystem::path& dir);
    void InvalidateParent(const std::filesystem::path& path) {
        Invalidate(path.parent_path());
    }

private:
    struct Listing {
        bool exists{};
        std::unordered_map<std::string, std::string> names; // lowercase -> actual
    };
    const Listing& Get(const std::filesystem::path& dir);

    std::mutex mutex;
    std::unordered_map<std::string, Listing> dirs;
};

} // namespace Core::FileSys
