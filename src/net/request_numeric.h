// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "net/request_values.h"
#include <limits>
#include <type_traits>

namespace rta::net {
// HTTP/JSON numeric boundary conversion. Validate before narrowing; never round or clamp.
// Target bounds must be exactly representable in the JSON parser's double storage.
template<class T>
inline T request_integral(double value, const char* key) {
    static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>, "integer target required");
    static_assert(std::numeric_limits<T>::digits <= std::numeric_limits<double>::digits,
                  "target bounds must be exactly representable as double");
    if (!std::isfinite(value) || std::trunc(value) != value ||
        value < static_cast<double>(std::numeric_limits<T>::lowest()) ||
        value > static_cast<double>(std::numeric_limits<T>::max()))
        throw InvalidRequest(std::string("Invalid integer parameter: ") + key);
    return static_cast<T>(value);
}
} // namespace rta::net
