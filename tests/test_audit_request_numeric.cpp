// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/request_numeric.h"
#include "net/json.h"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
static int checks = 0;
static void check(bool pass) {
    ++checks;
    if (!pass) throw std::runtime_error("request numeric check " + std::to_string(checks));
}
template<class T> static void rejected(double value) {
    bool invalid = false;
    try { (void)rta::net::request_integral<T>(value, "value"); }
    catch (const rta::net::InvalidRequest&) { invalid = true; }
    check(invalid);
}
template<class T> static void bounds() {
    using namespace rta::net;
    const double low = static_cast<double>(std::numeric_limits<T>::lowest());
    const double high = static_cast<double>(std::numeric_limits<T>::max());
    check(request_integral<T>(0, "value") == T{0});
    check(request_integral<T>(-0.0, "value") == T{0});
    check(request_integral<T>(low, "value") == std::numeric_limits<T>::lowest());
    check(request_integral<T>(high, "value") == std::numeric_limits<T>::max());
    rejected<T>(low - 1);
    rejected<T>(high + 1);
    for (double invalid : {0.5, -0.5, high + 0.5, 1e300, -1e300,
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) rejected<T>(invalid);
}
int main() {
    using namespace rta::net;
    bounds<uint16_t>(); bounds<uint32_t>(); bounds<int>();
    check(request_integral<int>(-1, "bits") == -1); // Type range only; product rules remain at the caller.
    rejected<uint16_t>(-1); rejected<uint32_t>(-1);
    rejected<uint32_t>(json_parse("1e300").n());
    check(request_integral<uint32_t>(json_parse("4294967295").n(), "rate") == UINT32_MAX);
    check(request_integral<uint16_t>(json_parse("65535").n(), "port") == UINT16_MAX);
    check(request_integral<int>(json_parse("-2147483648").n(), "bits") == INT32_MIN);
    rejected<int>(json_parse("2147483648").n());
    rejected<uint32_t>(json_parse("4294967296").n());
    std::cout << "PASS request numeric " << checks << " checks\n";
}
