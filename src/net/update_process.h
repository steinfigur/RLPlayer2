// SPDX-License-Identifier: GPL-3.0-or-later
// Installation lifetime policy: running servers share a lease; replacement requires exclusivity.
// Native lock and process inspection primitives live in platform/update_support.h.
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "platform/update_support.h"
#include "platform/path.h"

namespace rta::update {
class InstallRuntimeGuard {
    std::unique_ptr<rta::platform::FileLease> lease_;
    std::string error_;
public:
    InstallRuntimeGuard(const std::string& install, bool exclusive) {
        std::error_code ec;
        const auto dir = rta::platform::from_utf8(install) / "updates";
        std::filesystem::create_directories(dir, ec);
        if (ec) { error_ = "Cannot create update lease directory: " + ec.message(); return; }
        lease_ = std::make_unique<rta::platform::FileLease>(dir / "runtime.lock", exclusive
            ? rta::platform::FileLeaseMode::RuntimeExclusive : rta::platform::FileLeaseMode::RuntimeShared);
        if (!lease_->ok()) error_ = "Installation is in use or cannot be locked (Win32 " + std::to_string(lease_->error_code()) + ")";
    }
    InstallRuntimeGuard(const InstallRuntimeGuard&) = delete;
    InstallRuntimeGuard& operator=(const InstallRuntimeGuard&) = delete;
    bool ok() const { return lease_ && lease_->ok(); }
    const std::string& error() const { return error_; }
};
inline bool install_has_running_server(const std::string& install, std::string& error) {
    return rta::platform::executable_path_is_running(rta::platform::from_utf8(install) / "rtaweb.exe", error);
}
} // namespace rta::update
