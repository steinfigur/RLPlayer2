// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <charconv>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>

namespace rta::net {
class InvalidRequest : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};
inline double request_number(std::string_view value, const char* key) {
    double result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
        !std::isfinite(result)) throw InvalidRequest(std::string("Invalid numeric parameter: ") + key);
    return result;
}
inline int request_integer(std::string_view value, const char* key) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (value.empty() || parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
        throw InvalidRequest(std::string("Invalid integer parameter: ") + key);
    return result;
}
inline std::string_view request_path(std::string_view target) {
    return target.substr(0, target.find('?'));
}
inline bool route_matches(std::string_view target, std::string_view route, bool subtree = false) {
    const auto path = request_path(target);
    return path == route || (subtree && path.size() > route.size() &&
        path.substr(0, route.size()) == route && path[route.size()] == '/');
}
} // namespace rta::net
