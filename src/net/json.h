// json.h — 严格 JSON 只读解析（错误不返回部分结果）
//
// 用途：REST 响应与持久化缓存。拒绝截断、非法数字、转义及非标 UTF-8。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cmath>
#include <sstream>
#include <locale>
#include <limits>
#include <iomanip>

namespace rta::net {

struct Json {
    enum Type { Null, Bool, Num, Str, Arr, Obj } type = Null;
    bool   b = false;
    double num = 0.0;
    std::string str;
    std::vector<Json> arr;
    std::map<std::string, Json> obj;

    bool has(const std::string& k) const { return type == Obj && obj.count(k) > 0; }
    const Json& operator[](const std::string& k) const {
        static const Json nil;
        auto it = obj.find(k);
        return (type == Obj && it != obj.end()) ? it->second : nil;
    }
    const Json& at(size_t i) const {
        static const Json nil;
        return (type == Arr && i < arr.size()) ? arr[i] : nil;
    }
    size_t size() const { return type == Arr ? arr.size() : 0; }
    std::string s() const {
        if (type == Str) return str;
        if (type != Num) return {};
        // Preserve the existing integer/truncation form only while conversion is representable.
        const double lower = static_cast<double>(std::numeric_limits<long long>::lowest());
        if (num >= lower && num < -lower) return std::to_string(static_cast<long long>(num));
        std::ostringstream stream;
        stream.imbue(std::locale::classic());
        stream << std::setprecision(std::numeric_limits<double>::max_digits10) << num;
        return stream.str();
    }
    double n() const { return type == Num ? num : (type == Str ? atof(str.c_str()) : 0.0); }
    bool ok() const { return type != Null; }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& s) : s_(s) {}
    Json parse() {
        p_ = 0; error_.clear();
        Json j;
        if (!value(j, 0)) return Json{};
        skip();
        if (p_ != s_.size()) { fail("trailing data"); return Json{}; }
        return j;
    }
    bool valid() const { return error_.empty(); }
    const std::string& error() const { return error_; }
    size_t position() const { return p_; }
private:
    const std::string& s_;
    size_t p_ = 0;
    std::string error_;
    bool fail(const char* message) { if (error_.empty()) error_ = message; return false; }
    void skip() {
        while (p_ < s_.size() && (s_[p_] == ' ' || s_[p_] == '\t' || s_[p_] == '\r' || s_[p_] == '\n')) ++p_;
    }
    bool eat(char c) { skip(); if (p_ < s_.size() && s_[p_] == c) { ++p_; return true; } return false; }
    bool hex4(unsigned& cp) {
        cp = 0;
        for (int i = 0; i < 4; ++i) {
            if (p_ == s_.size()) return fail("truncated unicode escape");
            const unsigned char c = (unsigned char)s_[p_++];
            unsigned v;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            else return fail("invalid unicode escape");
            cp = cp * 16 + v;
        }
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += (char)cp;
        else if (cp < 0x800) { out += (char)(0xc0 | (cp >> 6)); out += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { out += (char)(0xe0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
        else { out += (char)(0xf0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 63)); out += (char)(0x80 | ((cp >> 6) & 63)); out += (char)(0x80 | (cp & 63)); }
    }
    bool str(std::string& out) {
        if (!eat('"')) return fail("expected string");
        while (p_ < s_.size()) {
            const unsigned char c = (unsigned char)s_[p_++];
            if (c == '"') return true;
            if (c < 0x20) return fail("unescaped control character");
            if (c == '\\') {
                if (p_ == s_.size()) return fail("truncated escape");
                const char e = s_[p_++];
                switch (e) {
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case '/': out += '/'; break;
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case 'u': {
                        unsigned cp;
                        if (!hex4(cp)) return false;
                        if (cp >= 0xd800 && cp <= 0xdbff) {
                            if (p_ + 2 > s_.size() || s_[p_] != '\\' || s_[p_ + 1] != 'u') return fail("missing low surrogate");
                            p_ += 2;
                            unsigned low;
                            if (!hex4(low)) return false;
                            if (low < 0xdc00 || low > 0xdfff) return fail("invalid low surrogate");
                            cp = 0x10000 + ((cp - 0xd800) << 10) + low - 0xdc00;
                        } else if (cp >= 0xdc00 && cp <= 0xdfff) return fail("isolated low surrogate");
                        utf8(out, cp); break;
                    }
                    default: return fail("invalid escape");
                }
            } else if (c < 0x80) out += (char)c;
            else {
                unsigned cp; int count; unsigned min;
                if (c >= 0xc2 && c <= 0xdf) { cp = c & 31; count = 1; min = 0x80; }
                else if (c >= 0xe0 && c <= 0xef) { cp = c & 15; count = 2; min = 0x800; }
                else if (c >= 0xf0 && c <= 0xf4) { cp = c & 7; count = 3; min = 0x10000; }
                else return fail("invalid UTF-8");
                for (int i = 0; i < count; ++i) {
                    if (p_ == s_.size()) return fail("truncated UTF-8");
                    const unsigned char cc = (unsigned char)s_[p_++];
                    if ((cc & 0xc0) != 0x80) return fail("invalid UTF-8 continuation");
                    cp = (cp << 6) | (cc & 63);
                }
                if (cp < min || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return fail("invalid UTF-8 codepoint");
                utf8(out, cp);
            }
        }
        return fail("unterminated string");
    }
    bool digit() const { return p_ < s_.size() && s_[p_] >= '0' && s_[p_] <= '9'; }
    bool value(Json& j, unsigned depth) {
        if (depth > 256) return fail("maximum nesting exceeded");
        skip();
        if (p_ == s_.size()) return fail("expected value");
        const char c = s_[p_];
        if (c == '{') {
            ++p_; j.type = Json::Obj;
            if (eat('}')) return true;
            for (;;) {
                std::string k;
                if (!str(k) || !eat(':')) return fail("expected object colon");
                Json v; if (!value(v, depth + 1)) return false;
                j.obj[k] = std::move(v);
                if (eat('}')) return true;
                if (!eat(',')) return fail("expected object close or comma");
            }
        }
        if (c == '[') {
            ++p_; j.type = Json::Arr;
            if (eat(']')) return true;
            for (;;) {
                Json v; if (!value(v, depth + 1)) return false;
                j.arr.push_back(std::move(v));
                if (eat(']')) return true;
                if (!eat(',')) return fail("expected array close or comma");
            }
        }
        if (c == '"') { j.type = Json::Str; return str(j.str); }
        if (c == 't' || c == 'f' || c == 'n') {
            const std::string token = c == 't' ? "true" : c == 'f' ? "false" : "null";
            if (s_.compare(p_, token.size(), token) != 0) return fail("invalid literal");
            p_ += token.size();
            j.type = c == 'n' ? Json::Null : Json::Bool; j.b = c == 't';
            return true;
        }
        const size_t start = p_;
        if (s_[p_] == '-') ++p_;
        if (!digit()) return fail("expected number");
        if (s_[p_] == '0') ++p_;
        else while (digit()) ++p_;
        if (p_ < s_.size() && s_[p_] == '.') { ++p_; if (!digit()) return fail("expected fraction"); while (digit()) ++p_; }
        if (p_ < s_.size() && (s_[p_] == 'e' || s_[p_] == 'E')) {
            ++p_; if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) ++p_;
            if (!digit()) return fail("expected exponent");
            while (digit()) ++p_;
        }
        j.type = Json::Num;
        const std::string number = s_.substr(start, p_ - start);
        // Classic locale keeps JSON's decimal point independent of the process locale.
        std::istringstream stream(number); stream.imbue(std::locale::classic());
        stream >> j.num;
        if (stream.fail() || !std::isfinite(j.num)) return fail("number out of range");
        return true;
    }
};

inline Json json_parse(const std::string& text, std::string* error = nullptr, size_t* offset = nullptr) {
    JsonParser parser(text);
    Json j = parser.parse();
    if (error) *error = parser.error();
    if (offset) *offset = parser.position();
    return j;
}

} // namespace rta::net
