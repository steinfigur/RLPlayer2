// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/request_values.h"
#include <iostream>
#include <cstdlib>
using namespace rta::net;
static int checks = 0;
static void require(bool pass) { ++checks; if (!pass) { std::cerr << "FAIL " << checks << '\n'; std::exit(1); } }
int main() {
    for (const char* bad : {"", "nan", "NaN", "inf", "-inf", "1e999", "2junk", " 1", "1 ", "1,2"}) {
        bool rejected = false; try { (void)request_number(bad, "volume"); } catch (const InvalidRequest&) { rejected=true; }
        require(rejected);
    }
    for (const char* bad : {"", "1.0", "nan", "2147483648", "-2147483649", "3x"}) {
        bool rejected=false; try { (void)request_integer(bad, "channels"); } catch (const InvalidRequest&) { rejected=true; }
        require(rejected);
    }
    require(request_number("-120", "db") == -120); require(request_number("0", "db") == 0);
    require(request_number("-1.25e1", "db") == -12.5); require(request_integer("8", "channels") == 8);
    for (const char* route : {"/api/quit", "/api/play", "/api/volume", "/api/lib/cover/prefetch", "/api/settings"}) {
        const std::string p(route);
        require(route_matches(p, route)); require(route_matches(p+"?x=1", route));
        require(!route_matches(p+"XYZ", route)); require(!route_matches(p+"/x", route));
        require(!route_matches(p+"%3f", route)); require(!route_matches(p+"&x=1", route));
        require(route_matches(p+"/x", route, true)); require(!route_matches(p+"XYZ", route, true));
    }
    require(!route_matches("/api/playmode", "/api/play", true));
    require(!route_matches("/api/lib/cover/prefetch", "/api/lib/cover"));
    require(route_matches("/api/lib/cover/prefetch?x=1", "/api/lib", true));
    std::cout << "[PASS] request contract checks=" << checks << '\n';
}
