// Windows implementation of feedback_support.h. Read-only host APIs and WinHTTP only.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <winhttp.h>
#include <algorithm>
#include <cstdlib>
#include "crash_dump.h" // Reuse OS/CPU observations; never install a crash handler here.
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "user32.lib")

namespace rta::platform {
namespace feedback_detail {
inline std::string winhttp_reason(DWORD e) {
    switch (e) {
        case 12001: return "内存/缓冲区不足";
        case 12002: return "请求超时（网络太慢或被墙）";
        case 12005: return "URL 不合法";
        case 12006: return "网络不可用";
        case 12007: return "无法解析主机名（DNS 失败，检查网络）";
        case 12008: return "代理或服务器地址被拒绝";
        case 12017: return "操作被取消";
        case 12019: return "连接被服务器断开";
        case 12029: return "无法连接服务器（防火墙/代理/断网）";
        case 12030: return "连接被拒绝或中断";
        case 12152: return "服务器返回了无法解析的响应";
        case 12175: return "TLS/证书校验失败";
        default:     return "WinHTTP 错误 " + std::to_string((unsigned long)e);
    }
}

struct UrlParts {
    bool https = false;
    std::wstring host;
    int port = 0;
    std::wstring path;
    bool valid = false;
};

inline std::wstring utf8_to_wide(const std::string& s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

inline UrlParts parse_url(const std::string& url) {
    UrlParts u;
    size_t p = 0;
    if (url.rfind("https://", 0) == 0) { u.https = true; p = 8; }
    else if (url.rfind("http://", 0) == 0) { u.https = false; p = 7; }
    else return u;
    const size_t slash = url.find('/', p);
    const std::string hp = (slash == std::string::npos) ? url.substr(p) : url.substr(p, slash - p);
    const std::string path = (slash == std::string::npos) ? "/" : url.substr(slash);
    std::string host = hp;
    int port = u.https ? 443 : 80;
    const size_t colon = hp.rfind(':');
    if (colon != std::string::npos) {
        host = hp.substr(0, colon);
        port = std::atoi(hp.c_str() + colon + 1);
    }
    if (host.empty() || port <= 0) return u;
    u.host = utf8_to_wide(host);
    u.path = utf8_to_wide(path);
    u.port = port;
    u.valid = !u.host.empty() && !u.path.empty();
    return u;
}

} // namespace feedback_detail

inline FeedbackLocalClock feedback_local_clock() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    TIME_ZONE_INFORMATION tzi{};
    const DWORD tz = GetTimeZoneInformation(&tzi);
    const long bias = tzi.Bias + (tz == TIME_ZONE_ID_DAYLIGHT ? tzi.DaylightBias : tzi.StandardBias);
    return {st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, -bias};
}
inline unsigned long long process_id() { return GetCurrentProcessId(); }
inline unsigned processor_count() {
    SYSTEM_INFO si{};
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors;
}
inline std::string host_os_description() { return rta::diag::os_version_string(); }
inline std::string processor_brand() { return rta::diag::cpu_brand(); }
inline std::string feedback_environment_value(const std::string& name) {
    const auto key = feedback_detail::utf8_to_wide(name);
    wchar_t buf[1024];
    const DWORD n = ::GetEnvironmentVariableW(key.c_str(), buf, 1024);
    if (n == 0 || n >= 1024) return "";
    const int m = ::WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, nullptr, 0, nullptr, nullptr);
    if (m <= 0) return "";
    std::string value((size_t)m, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buf, (int)n, value.data(), m, nullptr, nullptr);
    return value;
}

inline std::vector<std::string> desktop_display_names() {
    std::vector<std::string> names;
    for (DWORD i = 0; i < 8; ++i) {
        DISPLAY_DEVICEW dd{};
        dd.cb = sizeof(dd);
        if (!EnumDisplayDevicesW(nullptr, i, &dd, 0)) break;
        if (!(dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP)) continue;
        char u8[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, dd.DeviceString, -1, u8, (int)sizeof(u8) - 1, nullptr, nullptr);
        const std::string n(u8);
        if (n.empty()) continue;
        if (std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    }
    return names;
}

// A running process has no exit time: subtract creation time from the current clock.
inline unsigned long long process_uptime_seconds() {
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER cur, born;
    cur.LowPart = now.dwLowDateTime; cur.HighPart = now.dwHighDateTime;
    born.LowPart = c.dwLowDateTime;  born.HighPart = c.dwHighDateTime;
    if (cur.QuadPart <= born.QuadPart) return 0;
    return (unsigned long long)((cur.QuadPart - born.QuadPart) / 10000000ULL);
}

inline FeedbackMemoryInfo feedback_memory_info() {
    FeedbackMemoryInfo m;
    PROCESS_MEMORY_COUNTERS_EX pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        m.wsMb   = (unsigned long long)pmc.WorkingSetSize / (1024ull * 1024ull);
        m.peakMb = (unsigned long long)pmc.PeakWorkingSetSize / (1024ull * 1024ull);
        m.privMb = (unsigned long long)pmc.PrivateUsage / (1024ull * 1024ull);
    }
    DWORD h = 0;
    if (GetProcessHandleCount(GetCurrentProcess(), &h)) m.handles = (unsigned)h;
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
        m.ramTotalMb = (unsigned long long)ms.ullTotalPhys / (1024ull * 1024ull);
        m.ramAvailMb = (unsigned long long)ms.ullAvailPhys / (1024ull * 1024ull);
    }
    return m;
}

inline FeedbackHttpResult feedback_http_request(const std::string& method, const std::string& url, const std::vector<std::string>& headers,
                               const std::string& body, int timeoutMs, int proxyMode,
                               const std::string& proxy) {
    FeedbackHttpResult r;
    const feedback_detail::UrlParts u = feedback_detail::parse_url(url);
    if (!u.valid) { r.error = "URL 解析失败"; return r; }

    const std::wstring wproxy = (proxyMode == 2) ? feedback_detail::utf8_to_wide(proxy) : std::wstring();
    const DWORD access = (proxyMode == 1) ? WINHTTP_ACCESS_TYPE_NO_PROXY
                       : (proxyMode == 2) ? WINHTTP_ACCESS_TYPE_NAMED_PROXY
                                          : WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
    HINTERNET ses = ::WinHttpOpen(L"RLPlayer-feedback/1.0", access,
                                  (proxyMode == 2) ? wproxy.c_str() : WINHTTP_NO_PROXY_NAME,
                                  WINHTTP_NO_PROXY_BYPASS, 0);
    if (!ses) { r.error = feedback_detail::winhttp_reason(::GetLastError()); return r; }
    ::WinHttpSetTimeouts(ses, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET con = ::WinHttpConnect(ses, u.host.c_str(), (INTERNET_PORT)u.port, 0);
    if (!con) { r.error = feedback_detail::winhttp_reason(::GetLastError()); ::WinHttpCloseHandle(ses); return r; }

    const std::wstring wmethod = feedback_detail::utf8_to_wide(method);
    HINTERNET hreq = ::WinHttpOpenRequest(con, wmethod.c_str(), u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                          WINHTTP_DEFAULT_ACCEPT_TYPES,
                                          u.https ? WINHTTP_FLAG_SECURE : 0);
    if (!hreq) {
        r.error = feedback_detail::winhttp_reason(::GetLastError());
        ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }
    for (const auto& h : headers) {
        const std::wstring w = feedback_detail::utf8_to_wide(h);
        if (!w.empty()) ::WinHttpAddRequestHeaders(hreq, w.c_str(), (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD);
    }
    const bool sent = ::WinHttpSendRequest(hreq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                           (LPVOID)body.data(), (DWORD)body.size(),
                                           (DWORD)body.size(), 0) != 0 &&
                      ::WinHttpReceiveResponse(hreq, nullptr) != 0;
    if (!sent) {
        r.error = feedback_detail::winhttp_reason(::GetLastError());
        ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
        return r;
    }
    DWORD code = 0, len = sizeof(code);
    ::WinHttpQueryHeaders(hreq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                          WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
    r.status = (int)code;
    for (;;) {
        DWORD avail = 0;
        if (!::WinHttpQueryDataAvailable(hreq, &avail) || avail == 0) break;
        std::vector<char> buf((size_t)avail);
        DWORD got = 0;
        if (!::WinHttpReadData(hreq, buf.data(), avail, &got) || got == 0) break;
        r.body.append(buf.data(), (size_t)got);
    }
    ::WinHttpCloseHandle(hreq); ::WinHttpCloseHandle(con); ::WinHttpCloseHandle(ses);
    r.ok = (r.status >= 200 && r.status < 300);
    if (!r.ok && r.error.empty()) r.error = "HTTP " + std::to_string(r.status);
    return r;
}

} // namespace rta::platform
