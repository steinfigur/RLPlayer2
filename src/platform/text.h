// Text encoding primitives. Explicit lengths preserve embedded NULs; malformed
// UTF-16/UTF-8 uses the platform's replacement behavior, matching the legacy readers.
#pragma once
#include <string>

namespace rta::platform {
std::string utf16_to_utf8(const std::wstring& text);
std::wstring utf8_to_utf16(const std::string& text);
// Strict CP936 sizing first, then replacement decoding; complete failure keeps bytes.
std::string gbk_to_utf8(const std::string& text);
} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/text_win32.h"
#else
#error "platform/text.h: this platform has no text encoding implementation yet (see PLATFORM.md)."
#endif
