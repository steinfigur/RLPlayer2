#include "net/json.h"
#include <iostream>
#include <vector>
#include <stdexcept>
static int checks = 0;
static void check(bool pass) { ++checks; if (!pass) throw std::runtime_error("JSON check " + std::to_string(checks)); }
int main() {
    using namespace rta::net;
    for (const std::string text : {"null", "true", "false", "{}", "[]", "-0", "1.5e+2", "[1,true,null,\"x\"]", "{\"x\":{\"n\":-1.25e-2}}"}) {
        JsonParser parser(text); parser.parse(); check(parser.valid());
    }
    const std::vector<std::string> invalid = {"", "{", "[", "{\"x\":", "{\"x\":\"partial", "{\"x\":[1,2", "[true", "[1,]", "{\"x\":1,}", "{x:1}", "[1 2]", "{\"a\" 1}", "00", "01", "+1", "-", ".1", "1.", "1e", "1e+", "1e999", "tru", "truth", "falsehood", "nil", "null x", "true false", "{}x", "\"\\q\"", "\"\\u12\"", "\"\\uZZZZ\"", "\"\\ud800\"", "\"\\udc00\"", "\"\\ud800\\u1234\"", "\"a\n\"", "\"\\", std::string("\"\xed\xa0\xbd\""), std::string("\"\xc0\x80\""), std::string("\"\xf4\x90\x80\x80\"")};
    for (const auto& text : invalid) {
        std::string error; size_t offset = 0;
        const auto j = json_parse(text, &error, &offset);
        check(j.type == Json::Null && !error.empty() && offset <= text.size());
    }
    check(json_parse("\"\\ud83d\\ude00\"").str == "\xf0\x9f\x98\x80");
    check(json_parse("\"\\ud840\\udc00\"").str == "\xf0\xa0\x80\x80");
    check(json_parse("\"\\u4e2d\\u6587\"").str == "中文");
    check(json_parse("\"😀\"").str == json_parse("\"\\ud83d\\ude00\"").str);
    std::string error; check(json_parse("null", &error).type == Json::Null && error.empty());
    check(json_parse(std::string(258, '[') + std::string(258, ']'), &error).type == Json::Null && !error.empty());
    check(json_parse("1.75").s() == "1");
    check(json_parse("-1.75").s() == "-1");
    check(json_parse("-9223372036854775808").s() == "-9223372036854775808");
    check(json_parse("9223372036854774784").s() == "9223372036854774784");
    for (const auto& number : {"9223372036854775807", "9223372036854775808", "-9223372036854777856", "1e300", "-1e300"}) {
        const auto parsed = json_parse(number);
        const auto text = parsed.s();
        check(!text.empty() && json_parse(text).type == Json::Num && json_parse(text).num == parsed.num);
    }
    std::cout << "PASS JSON " << checks << " checks\n";
}
