#pragma once
#include <filesystem>
#include <chrono>
#include <stdexcept>
#include <iostream>
inline std::string audit_fixture(const std::string& parent) {
    const auto root = std::filesystem::u8path(parent);
    std::filesystem::create_directories(root);
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    for (unsigned i = 0; ; ++i) {
        const auto path = root / ("run-" + std::to_string(stamp) + "-" + std::to_string(i));
        std::error_code ec;
        if (std::filesystem::create_directory(path, ec)) {
            const auto utf8 = path.u8string();
            const std::string result(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            std::cout << "fixture=" << result << "\n";
            return result;
        }
        if (ec) throw std::runtime_error("cannot create audit fixture: " + ec.message());
    }
}
