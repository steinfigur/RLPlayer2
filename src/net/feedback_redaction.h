// Shared semantic redaction for diagnostic JSON, settings and text.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <sstream>
#include <iomanip>
#include <limits>
#include <locale>
#include "net/json.h"

namespace rta::feedback {
inline bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) || c == '_';
}

// 非 UTF-8 字节 → U+FFFD（日志里混入 ANSI 代码页字节时，不至于让整份 JSON 变成非法 UTF-8）
inline std::string utf8_sanitize(const std::string& in) {
    std::string o;
    o.reserve(in.size() + 16);
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = (unsigned char)in[i];
        size_t need = 0;
        if (c < 0x80) need = 0;
        else if ((c & 0xE0) == 0xC0) need = 1;
        else if ((c & 0xF0) == 0xE0) need = 2;
        else if ((c & 0xF8) == 0xF0) need = 3;
        else { o += "\xEF\xBF\xBD"; ++i; continue; }
        if (i + need >= n) { o += "\xEF\xBF\xBD"; ++i; continue; }
        bool good = true;
        for (size_t k = 1; k <= need; ++k)
            if (((unsigned char)in[i + k] & 0xC0) != 0x80) { good = false; break; }
        if (!good) { o += "\xEF\xBF\xBD"; ++i; continue; }
        o.append(in, i, need + 1);
        i += need + 1;
    }
    return o;
}

inline std::string jesc(const std::string& raw) {
    const std::string s = utf8_sanitize(raw);
    std::string o;
    o.reserve(s.size() + 8);
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = (unsigned char)s[i];
        switch (c) {
            case '"' : o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\b': o += "\\b";  break;
            case '\f': o += "\\f";  break;
            case '\n': o += "\\n";  break;
            case '\r': o += "\\r";  break;
            case '\t': o += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", (unsigned)c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}

inline std::string base64_encode(const std::string& in) {
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((in.size() + 2) / 3 * 4 + 4);
    size_t i = 0;
    while (i + 2 < in.size()) {
        const unsigned v = ((unsigned)(unsigned char)in[i] << 16) |
                           ((unsigned)(unsigned char)in[i + 1] << 8) |
                           (unsigned)(unsigned char)in[i + 2];
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += T[v & 63];
        i += 3;
    }
    const size_t rest = in.size() - i;
    if (rest == 1) {
        const unsigned v = (unsigned)(unsigned char)in[i] << 16;
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += "==";
    } else if (rest == 2) {
        const unsigned v = ((unsigned)(unsigned char)in[i] << 16) | ((unsigned)(unsigned char)in[i + 1] << 8);
        o += T[(v >> 18) & 63]; o += T[(v >> 12) & 63]; o += T[(v >> 6) & 63]; o += '=';
    }
    return o;
}

// 不区分大小写的子串查找
inline bool ci_find(const std::string& s, const char* lit, size_t from, size_t& hit) {
    const size_t n = std::strlen(lit);
    if (n == 0 || s.size() < n) return false;
    for (size_t i = from; i + n <= s.size(); ++i) {
        size_t k = 0;
        for (; k < n; ++k)
            if (std::tolower((unsigned char)s[i + k]) != std::tolower((unsigned char)lit[k])) break;
        if (k == n) { hit = i; return true; }
    }
    return false;
}

inline void replace_range(std::string& s, size_t a, size_t b, const std::string& rep) {
    if (a >= b || b > s.size()) return;
    s.replace(a, b - a, rep);
}

// ---------------------------------------------------------------------------
// 脱敏（唯一入口：payload 里每个字符串都过它）
//
// 规则清单（也是 payload 里 redaction.rules 报给用户看的那几条）：
//   1) openai_key          —— "sk-" 开头的密钥（含 sk-proj- / sk-live- …）
//   2) github_token        —— ghp_ / gho_ / ghu_ / ghs_ / ghr_ / github_pat_ 开头的 token
//   3) aws_key / slack_token —— AKIA… / xoxb-… / xoxp-…
//   4) auth_header         —— Authorization: <值>、Proxy-Authorization:、X-Api-Key:、Cookie:
//   5) bearer_token        —— 裸的 "Bearer <token>"
//   6) credential_kv       —— api_key / apikey / token / access_token / password / secret /
//                             client_secret / auth / key / gh_token 后面跟的值
//                             （"k":"v" 与 k=v 两种写法都认）
//   7) windows_user_path   —— C:\Users\<用户名>\… 与 /Users/<用户名>/… 里的用户名 → <user>
//   8) email               —— 邮箱本地部分 → <user>，域名保留
// 被替换掉的一律写 <redacted>（用户目录名写 <user>）：前缀/键名保留，方便判断"当时配没配"。
//
// 已知的"宁多勿漏"取舍：规则 6 里的裸 key 会把日志中形如 key=<封面缓存键> 的普通键值
// （rtaweb 的 COVERBG 行）一并打码 —— 拒绝把"看起来像凭据"的东西发出去，代价是那一小段
// 调试信息丢失（同一行的 name= 仍保留，够定位是哪张封面）。
// ---------------------------------------------------------------------------

inline void mask_prefixed_tokens(std::string& s) {
    static const char* kPfx[] = {"sk-", "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_",
                                 "xoxb-", "xoxp-", "AKIA"};
    size_t scan = 0;
    for (;;) {
        size_t hit = std::string::npos, plen = 0;
        for (const char* pf : kPfx) {
            const size_t p = s.find(pf, scan);
            if (p != std::string::npos && (hit == std::string::npos || p < hit)) {
                hit = p; plen = std::strlen(pf);
            }
        }
        if (hit == std::string::npos) break;
        const bool leftOk = (hit == 0) || !is_ident_char(s[hit - 1]);
        size_t e = hit + plen;
        while (e < s.size() && (is_ident_char(s[e]) || s[e] == '-' || s[e] == '.')) ++e;
        const size_t vlen = e - (hit + plen);
        if (leftOk && vlen >= 6) { replace_range(s, hit, e, "<redacted>"); scan = hit + 10; }
        else scan = hit + plen;
    }
}

inline void mask_header_values(std::string& s) {
    static const char* kHdr[] = {"authorization", "proxy-authorization", "x-api-key", "api-key",
                                 "x-auth-token", "cookie"};
    size_t scan = 0;
    for (;;) {
        size_t hit = std::string::npos, hlen = 0;
        for (const char* h : kHdr) {
            size_t p = 0;
            if (!ci_find(s, h, scan, p)) continue;
            if (hit == std::string::npos || p < hit) { hit = p; hlen = std::strlen(h); }
        }
        if (hit == std::string::npos) break;
        if (hit > 0 && is_ident_char(s[hit - 1])) { scan = hit + hlen; continue; }
        size_t j = hit + hlen;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j >= s.size() || s[j] != ':') { scan = hit + hlen; continue; }
        ++j;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        const size_t vstart = j;
        size_t e = j;
        while (e < s.size()) {
            const char c = s[e];
            if (c == '\r' || c == '\n' || c == '"' || c == ',' || c == '}' || c == ';') break;
            ++e;
        }
        size_t ee = e;
        while (ee > vstart && (s[ee - 1] == ' ' || s[ee - 1] == '\t')) --ee;
        if (ee > vstart && s.compare(vstart, ee - vstart, "<redacted>") != 0)
            replace_range(s, vstart, ee, "<redacted>");
        scan = vstart + 10;
    }
}

inline void mask_bearer(std::string& s) {
    size_t scan = 0;
    for (;;) {
        size_t hit = 0;
        if (!ci_find(s, "bearer", scan, hit)) break;
        if (hit > 0 && is_ident_char(s[hit - 1])) { scan = hit + 6; continue; }
        size_t j = hit + 6;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        size_t e = j;
        while (e < s.size() && (is_ident_char(s[e]) || s[e] == '-' || s[e] == '.' || s[e] == '+' ||
                                s[e] == '/' || s[e] == '=')) ++e;
        if (e - j >= 6) { replace_range(s, j, e, "<redacted>"); scan = j + 10; }
        else scan = hit + 6;
    }
}

inline void mask_kv(std::string& s, const char* keyLit) {
    const size_t klen = std::strlen(keyLit);
    size_t scan = 0;
    for (;;) {
        size_t hit = 0;
        if (!ci_find(s, keyLit, scan, hit)) break;
        // 左右边界：前后都不能是标识符字符（避免 hotkey / monkey 被误伤）
        const bool leftOk = (hit == 0) || !is_ident_char(s[hit - 1]);
        const size_t after = hit + klen;
        const bool rightOk = (after >= s.size()) || !(is_ident_char(s[after]) || s[after] == '-');
        if (!leftOk || !rightOk) { scan = after; continue; }
        size_t j = after;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j < s.size() && (s[j] == '"' || s[j] == '\'')) ++j;   // "key": ...
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        if (j >= s.size() || (s[j] != '=' && s[j] != ':')) { scan = after; continue; }
        ++j;
        while (j < s.size() && (s[j] == ' ' || s[j] == '\t')) ++j;
        bool quoted = false;
        if (j < s.size() && (s[j] == '"' || s[j] == '\'')) { quoted = true; ++j; }
        const size_t vstart = j;
        size_t e = j;
        while (e < s.size()) {
            const char c = s[e];
            if (quoted) { if (c == '"' || c == '\'' || c == '\r' || c == '\n') break; }
            else if (c == '&' || c == '"' || c == '\'' || c == ',' || c == '}' || c == ';' ||
                     c == '\r' || c == '\n' || c == ' ' || c == '\t') break;
            ++e;
        }
        if (e > vstart && s.compare(vstart, e - vstart, "<redacted>") != 0)
            replace_range(s, vstart, e, "<redacted>");
        scan = vstart + 10;
    }
}

inline void mask_user_paths(std::string& s) {
    static const char* kRoots[] = {"\\users\\", "/users/", "\\documents and settings\\"};
    size_t scan = 0;
    for (;;) {
        size_t hit = std::string::npos, rlen = 0;
        for (const char* r : kRoots) {
            size_t p = 0;
            if (!ci_find(s, r, scan, p)) continue;
            if (hit == std::string::npos || p < hit) { hit = p; rlen = std::strlen(r); }
        }
        if (hit == std::string::npos) break;
        const size_t start = hit + rlen;
        size_t e = start;
        while (e < s.size() && s[e] != '\\' && s[e] != '/' && s[e] != '"' && s[e] != '\'' &&
               s[e] != ' ' && s[e] != '\r' && s[e] != '\n') ++e;
        if (e > start && s.compare(start, e - start, "<user>") != 0) {
            replace_range(s, start, e, "<user>");
            scan = start + 6;
        } else {
            scan = hit + rlen + 1;
        }
    }
}

inline void mask_emails(std::string& s) {
    size_t at = 0;
    for (;;) {
        const size_t p = s.find('@', at);
        if (p == std::string::npos) break;
        size_t a = p;
        while (a > 0 && (is_ident_char(s[a - 1]) || s[a - 1] == '.' || s[a - 1] == '-' ||
                         s[a - 1] == '+' || s[a - 1] == '%')) --a;
        size_t b = p + 1;
        while (b < s.size() && (is_ident_char(s[b]) || s[b] == '.' || s[b] == '-')) ++b;
        const size_t dot = s.find('.', p + 1);
        const bool looksMail = (b > p + 2) && (dot != std::string::npos) && (dot < b) && (p - a >= 2);
        if (looksMail && s.compare(a, p - a, "<user>") != 0) {
            replace_range(s, a, p, "<user>");
            at = a + 7;
        } else {
            at = p + 1;
        }
    }
}

inline bool sensitive_field(const std::string& key) {
    std::string normalized;
    for (unsigned char c : key) if (std::isalnum(c)) normalized += (char)std::tolower(c);
    static const char* keys[] = {"apikey", "xapikey", "token", "accesstoken", "refreshtoken",
        "password", "passwd", "secret", "clientsecret", "auth", "authorization",
        "proxyauthorization", "cookie", "setcookie", "key", "ghtoken"};
    for (const char* candidate : keys) if (normalized == candidate) return true;
    return false;
}

inline void mask_url_credentials(std::string& s) {
    size_t scan = 0;
    while ((scan = s.find("://", scan)) != std::string::npos) {
        const size_t start = scan + 3;
        const size_t end = s.find_first_of("/\\?# \t\r\n\"'", start);
        const size_t at = s.find('@', start);
        if (at != std::string::npos && (end == std::string::npos || at < end)) {
            s.replace(start, at - start, "<redacted>");
            scan = start + 11;
        } else scan = start;
    }
}

// 公开入口：任何要进 payload 的文本都必须过它
inline std::string redact(const std::string& in) {
    std::string s = in;
    mask_prefixed_tokens(s);
    mask_header_values(s);
    mask_bearer(s);
    static const char* kKeys[] = {"api_key", "apikey", "api-key", "x-api-key", "token",
                                  "access_token", "refresh_token", "password", "passwd",
                                  "secret", "client_secret", "auth", "key", "gh_token"};
    for (const char* k : kKeys) mask_kv(s, k);
    mask_user_paths(s);
    mask_url_credentials(s);
    mask_emails(s);
    return s;
}

// 脱敏 + JSON 转义 + 加引号。payload 里所有字符串都从这里出去。
inline std::string jq(const std::string& raw) {
    return std::string("\"") + jesc(redact(raw)) + std::string("\"");
}

// ---------------------------------------------------------------------------
// 调用方塞进来的 JSON 片段也走同一条脱敏路：解析 → 每个字符串重新 jq() → 序列化
// ---------------------------------------------------------------------------
namespace detail {

inline void emit_value(const rta::net::Json& v, std::string& o, bool sensitive = false) {
    if (sensitive) { o += "\"<redacted>\""; return; }
    switch (v.type) {
        case rta::net::Json::Null: o += "null"; break;
        case rta::net::Json::Bool: o += (v.b ? "true" : "false"); break;
        case rta::net::Json::Num: {
            std::ostringstream number;
            number.imbue(std::locale::classic());
            number << std::setprecision(std::numeric_limits<double>::max_digits10) << v.num;
            o += number.str();
            break;
        }
        case rta::net::Json::Str: o += jq(v.str); break;
        case rta::net::Json::Arr: {
            o += '[';
            for (size_t i = 0; i < v.arr.size(); ++i) { if (i) o += ','; emit_value(v.arr[i], o); }
            o += ']';
            break;
        }
        case rta::net::Json::Obj: {
            o += '{';
            bool first = true;
            for (const auto& kv : v.obj) {
                if (!first) o += ',';
                first = false;
                o += jq(kv.first);
                o += ':';
                emit_value(kv.second, o, sensitive_field(kv.first));
            }
            o += '}';
            break;
        }
    }
}

} // namespace detail

inline std::string sanitize_json_fragment(const std::string& raw) {
    if (raw.find_first_not_of(" \t\r\n") == std::string::npos) return "{}";
    rta::net::Json j = rta::net::json_parse(raw);
    if (j.type == rta::net::Json::Null) return std::string("{\"_unparsed\":") + jq(raw) + "}";
    std::string o;
    detail::emit_value(j, o);
    return o;
}


// Same serialization used for the actual Worker request; diagnostics have already
// passed the payload redaction contract before reaching this transport boundary.
inline std::string worker_request_body(const std::string& title, const std::string& body,
    const std::string& version, const std::string& source, const std::string& diagnosticJson) {
    std::string text = title.empty() ? body : title + "\n\n" + body;
    if (text.size() > 16000) text = text.substr(0, 16000) + "\n…（已截断）";
    return "{\"text\":" + jq(text) + ",\"version\":" + jq(version) +
        ",\"os\":" + jq("Windows · " + source) + ",\"diag\":" + jq(base64_encode(diagnosticJson)) + "}";
}

} // namespace rta::feedback
