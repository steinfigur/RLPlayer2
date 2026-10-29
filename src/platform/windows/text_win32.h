#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <string>

namespace rta::platform {
inline std::string utf16_to_utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string result((size_t)n, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), n, nullptr, nullptr);
    return result;
}

inline std::wstring utf8_to_utf16(const std::string& text) {
    if (text.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring result((size_t)n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), result.data(), n);
    return result;
}

inline std::string gbk_to_utf8(const std::string& text) {
    if (text.empty()) return {};
    int n = ::MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) n = ::MultiByteToWideChar(936, 0, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) return text;
    std::wstring wide((size_t)n, L'\0');
    if (::MultiByteToWideChar(936, 0, text.data(), (int)text.size(), wide.data(), n) <= 0) return text;
    return utf16_to_utf8(wide);
}
} // namespace rta::platform
