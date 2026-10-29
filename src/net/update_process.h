// SPDX-License-Identifier: GPL-3.0-or-later
// Installation lifetime lease: running servers share it; replacement requires exclusivity.
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <filesystem>
#include <string>

namespace rta::update {
class InstallRuntimeGuard {
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::string error_;
public:
    InstallRuntimeGuard(const std::string& install, bool exclusive) {
        std::error_code ec;
        const auto dir = std::filesystem::u8path(install) / "updates";
        std::filesystem::create_directories(dir, ec);
        if (ec) { error_ = "Cannot create update lease directory: " + ec.message(); return; }
        const auto file = dir / "runtime.lock";
        handle_ = CreateFileW(file.c_str(), GENERIC_READ, exclusive ? 0 : FILE_SHARE_READ,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            error_ = "Installation is in use or cannot be locked (Win32 " + std::to_string(GetLastError()) + ")";
    }
    ~InstallRuntimeGuard() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    InstallRuntimeGuard(const InstallRuntimeGuard&) = delete;
    InstallRuntimeGuard& operator=(const InstallRuntimeGuard&) = delete;
    bool ok() const { return handle_ != INVALID_HANDLE_VALUE; }
    const std::string& error() const { return error_; }
};

// Older servers do not hold runtime.lock. Inspect their executable, without terminating them.
// Unknown identity is a refusal to update, never permission to replace a potentially live image.
inline bool install_has_running_server(const std::string& install, std::string& error) {
    const auto target = std::filesystem::u8path(install) / L"rtaweb.exe";
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) { error = "Cannot inspect running servers"; return true; }
    PROCESSENTRY32W entry{}; entry.dwSize = sizeof(entry);
    bool found = false;
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"rtaweb.exe") != 0) continue;
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
} // namespace rta::update
