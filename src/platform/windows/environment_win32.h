#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstddef>
#include <string>

namespace rta::platform {
inline std::size_t read_environment_utf16(const wchar_t* name, wchar_t* value, std::size_t capacity) {
    return (std::size_t)::GetEnvironmentVariableW(name, value, (DWORD)capacity);
}
} // namespace rta::platform
