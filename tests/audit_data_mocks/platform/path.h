#pragma once
#include <string>
#include <cstdlib>
namespace rta::platform {
inline std::string join(const std::string& a, const std::string& b) { return a + "/" + b; }
inline std::string data_dir_utf8() { const char* p = std::getenv("RTA_AUDIT_SOURCE_ROOT"); return p ? p : ""; }
inline std::string config_dir_utf8() { return data_dir_utf8(); }
}
