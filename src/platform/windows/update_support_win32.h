// SPDX-License-Identifier: GPL-3.0-or-later
// Native filesystem durability, file leases and process/window inspection only.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include "platform/path.h"
#include "platform/fs.h"

namespace rta::platform {
struct FileLease::Impl {
    HANDLE handle = INVALID_HANDLE_VALUE;
    int error = 0;
    ~Impl() { if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle); }
};
inline FileLease::FileLease(const std::filesystem::path& file, FileLeaseMode mode)
    : impl_(std::make_unique<Impl>()) {
    const DWORD access = mode == FileLeaseMode::WriterExclusive ? GENERIC_READ | GENERIC_WRITE : GENERIC_READ;
    const DWORD share = mode == FileLeaseMode::RuntimeShared ? FILE_SHARE_READ : 0;
    impl_->handle = ::CreateFileW(file.c_str(), access, share, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (impl_->handle == INVALID_HANDLE_VALUE) impl_->error = (int)::GetLastError();
}
inline FileLease::~FileLease() = default;
inline bool FileLease::ok() const { return impl_->handle != INVALID_HANDLE_VALUE; }
inline bool FileLease::contended() const { return impl_->error == ERROR_SHARING_VIOLATION || impl_->error == ERROR_LOCK_VIOLATION; }
inline int FileLease::error_code() const { return impl_->error; }
inline bool reset_file_attributes(const std::filesystem::path& path) {
    return ::SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL) != 0;
}
inline bool durable_replace_file(const std::filesystem::path& from, const std::filesystem::path& to) {
    return ::MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}
inline std::string system_tool_path(const std::string& name) {
    wchar_t system[MAX_PATH] = {0};
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const auto file = std::filesystem::path(system) / from_utf8(name);
        if (file_exists(file)) return to_utf8(file);
    }
    return name;
}
inline bool request_update_launcher_exit() {
    HWND window = ::FindWindowW(L"RLPlayerTray", nullptr);
    if (!window) window = ::FindWindowW(L"RLPlayerTray", L"RLPlayerTray");
    if (!window) return false;
    ::PostMessageW(window, WM_COMMAND, (WPARAM)1003, 0);
    return true;
}
inline std::tm update_local_time(std::time_t time) {
    std::tm value{};
    localtime_s(&value, &time);
    return value;
}

inline bool executable_path_is_running(const std::filesystem::path& target, std::string& error) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { error = "Cannot inspect running servers"; return true; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, target.filename().c_str()) != 0) continue;
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, entry.th32ProcessID);
        if (!process) {
            if (GetLastError() == ERROR_INVALID_PARAMETER) continue; // exited since snapshot
            error = "Cannot inspect an active rtaweb process"; found = true; break;
        }
        wchar_t path[32768]; DWORD length = 32768;
        const bool queried = QueryFullProcessImageNameW(process, 0, path, &length) != 0;
        CloseHandle(process);
        if (!queried) { error = "Cannot resolve an active server's installation"; found = true; break; }
        std::error_code ec;
        const auto normalizedTarget = std::filesystem::weakly_canonical(target, ec);
        if (ec) { error = "Cannot resolve installation path"; found = true; break; }
        const auto normalizedProcess = std::filesystem::weakly_canonical(std::filesystem::path(path), ec);
        if (ec) { error = "Cannot resolve server path"; found = true; break; }
        bool same = _wcsicmp(normalizedTarget.c_str(), normalizedProcess.c_str()) == 0;
        if (!same && std::filesystem::exists(target, ec))
            same = std::filesystem::equivalent(target, std::filesystem::path(path), ec);
        if (same || ec) {
            error = same ? "An rtaweb process from this installation is still running" :
                           "Cannot verify that an active server belongs to another installation";
            found = true; break;
        }
    }
    CloseHandle(snapshot); return found;
}
} // namespace rta::platform
