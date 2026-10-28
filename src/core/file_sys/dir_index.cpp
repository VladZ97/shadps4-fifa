// SPDX-License-Identifier: GPL-2.0-or-later
#include <system_error>
#include "common/string_util.h"
#include "core/file_sys/dir_index.h"

namespace Core::FileSys {

DirIndex& DirIndex::Instance() {
    static DirIndex instance;
    return instance;
}

const DirIndex::Listing& DirIndex::Get(const std::filesystem::path& dir) {
    const auto key = dir.string();
    if (const auto it = dirs.find(key); it != dirs.end()) {
        return it->second;
    }
    Listing listing{};
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec)) {
        listing.exists = true;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            const auto name = entry.path().filename().string();
            listing.names.emplace(Common::ToLower(name), name);
        }
    }
    return dirs.emplace(key, std::move(listing)).first->second;
}

std::optional<std::filesystem::path> DirIndex::Lookup(const std::filesystem::path& dir,
                                                      std::string_view name) {
    std::scoped_lock lk{mutex};
    const auto& listing = Get(dir);
    if (!listing.exists) {
        return std::nullopt;
    }
    const auto it = listing.names.find(Common::ToLower(std::string{name}));
    if (it == listing.names.end()) {
        return std::nullopt;
    }
    return std::filesystem::path{it->second};
}

void DirIndex::Invalidate(const std::filesystem::path& dir) {
    std::scoped_lock lk{mutex};
    dirs.erase(dir.string());
}

} // namespace Core::FileSys
