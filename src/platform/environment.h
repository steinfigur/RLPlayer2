// Read the process environment without the CRT's shared getenv buffer.
#pragma once
#include <cstddef>
#include <string>

namespace rta::platform {
// Capacity includes the terminator. Returns copied length, zero for absent/empty,
// or required capacity (including terminator) when the buffer is too small.
std::size_t read_environment_utf16(const wchar_t* name, wchar_t* value, std::size_t capacity);
} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/environment_win32.h"
#else
#error "platform/environment.h: this platform has no environment implementation yet (see PLATFORM.md)."
#endif
