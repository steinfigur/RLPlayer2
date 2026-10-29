// Read-only host observations and one-shot transport used by diagnostic feedback.
// No feedback schema, credentials policy, JSON, redaction, retry or upload orchestration.
#pragma once
#include <string>
#include <vector>

namespace rta::platform {

struct FeedbackLocalClock {
    unsigned year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    long eastMinutes = 0;
};
FeedbackLocalClock feedback_local_clock();
unsigned long long process_id();
unsigned long long process_uptime_seconds();
unsigned processor_count();
std::string host_os_description();
std::string processor_brand();
std::vector<std::string> desktop_display_names();
// UTF-8 value; missing, oversized (>=1024 native characters) or conversion failure = empty.
std::string feedback_environment_value(const std::string& name);

struct FeedbackMemoryInfo {
    unsigned long long wsMb = 0, peakMb = 0, privMb = 0, ramTotalMb = 0, ramAvailMb = 0;
    unsigned handles = 0;
};

FeedbackMemoryInfo feedback_memory_info();

struct FeedbackHttpResult {
    bool ok = false;
    int status = 0;
    std::string error;
    std::string body;
};
// Blocking request, raw UTF-8 URL path, headers added verbatim, no retry.
// proxyMode: 0 = automatic system proxy, 1 = direct, 2 = named proxy.
// Each native operation gets timeoutMs. Non-2xx preserves the response body.
FeedbackHttpResult feedback_http_request(const std::string& method, const std::string& url,
    const std::vector<std::string>& headers, const std::string& body,
    int timeoutMs, int proxyMode, const std::string& proxy);

} // namespace rta::platform

#if defined(_WIN32)
#include "platform/windows/feedback_support_win32.h"
#else
#error "platform/feedback_support.h: host observations and feedback transport are not implemented on this platform"
#endif
