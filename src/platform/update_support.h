// SPDX-License-Identifier: GPL-3.0-or-later
// Native primitives used by update IO. Version, staging, journal and rollback policy live in net/.
#pragma once
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>

namespace rta::platform {
enum class FileLeaseMode { WriterExclusive, RuntimeShared, RuntimeExclusive };
class FileLease {
public:
    FileLease(const std::filesystem::path& file, FileLeaseMode mode);
    ~FileLease();
    FileLease(const FileLease&) = delete;
    FileLease& operator=(const FileLease&) = delete;
    bool ok() const;
    bool contended() const;
    int error_code() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
bool reset_file_attributes(const std::filesystem::path& path);
bool durable_replace_file(const std::filesystem::path& from, const std::filesystem::path& to);
std::string system_tool_path(const std::string& name);
bool executable_path_is_running(const std::filesystem::path& target, std::string& error);
bool request_update_launcher_exit();
std::tm update_local_time(std::time_t time);
} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/update_support_win32.h"
#else
#error "platform/update_support.h: implement native update IO for this platform"
#endif
