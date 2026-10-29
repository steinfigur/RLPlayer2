// Offline regression for platform extraction: no audio, processes or network.
#include "meta/cue.h"
#include "meta/tags_native.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>

static int failures = 0, checks = 0;
static void check(bool ok, const char* label) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", label); }
}

static std::string legacy_gbk(const std::string& text) {
    if (text.empty()) return {};
    int n = ::MultiByteToWideChar(936, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) n = ::MultiByteToWideChar(936, 0, text.data(), (int)text.size(), nullptr, 0);
    if (n <= 0) return text;
    std::wstring wide((size_t)n, L'\0');
    if (::MultiByteToWideChar(936, 0, text.data(), (int)text.size(), wide.data(), n) <= 0) return text;
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out((size_t)bytes, '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), (int)wide.size(), out.data(), bytes, nullptr, nullptr);
    return out;
}

int main() {
    using namespace rta::meta;
    namespace native = rta::meta::native;
    const std::string chinese = "\xe4\xb8\xad\xe6\x96\x87";
    const std::string gbk = "\xd6\xd0\xce\xc4";
    check(cue_gbk_to_utf8(gbk) == chinese, "CP936 Chinese to UTF8");
    check(cue_decode_text(gbk) == chinese, "CUE rejects GBK as UTF8 and decodes it");
    check(cue_decode_text("\xef\xbb\xbf" + chinese) == chinese, "UTF8 BOM");
    check(cue_decode_text(std::string("\xff\xfe\x2d\x4e\x87\x65", 6)) == chinese, "UTF16 LE CUE");
    check(cue_decode_text(std::string("\xfe\xff\x4e\x2d\x65\x87", 6)) == chinese, "UTF16 BE CUE");
    const std::string nul("A\0B", 3);
    check(cue_from_wstring(cue_widen_utf8(nul)) == nul, "explicit length preserves embedded NUL");
    check(cue_gbk_to_utf8({}).empty(), "empty GBK");
    for (const auto& text : {std::string("\x81", 1), std::string("A\xff\xfeZ", 4), std::string("A\0B", 3)})
        check(cue_gbk_to_utf8(text) == legacy_gbk(text), "bad CP936 bytes preserve legacy decoding");
    std::string converted;
    const unsigned char utf16le[] = {0x3d, 0xd8, 0x00, 0xde};
    const unsigned char utf16be[] = {0xd8, 0x3d, 0xde, 0x00};
    check(native::detail::utf16_to_utf8(utf16le, 4, false, &converted) && converted == "\xf0\x9f\x98\x80", "tag UTF16 LE surrogate pair");
    check(native::detail::utf16_to_utf8(utf16be, 4, true, &converted) && converted == "\xf0\x9f\x98\x80", "tag UTF16 BE surrogate pair");
    check(!native::detail::utf16_to_utf8(utf16le, 3, false, &converted) && converted.empty(), "odd UTF16 length rejected");
    const unsigned char bad16[] = {0x00, 0xd8};
    check(native::detail::utf16_to_utf8(bad16, 2, false, &converted) && converted == "\xef\xbf\xbd", "legacy unpaired surrogate replacement");
    check(native::detail::utf16_to_utf8(bad16, 0, false, &converted) && converted.empty(), "empty tag UTF16");

    struct SwitchCase { const wchar_t* value; bool nativeOn, guessOn, fillOn; };
    const SwitchCase cases[] = {{nullptr,true,true,true}, {L"",true,true,true}, {L"0",false,false,false},
        {L"OFF",false,false,false}, {L"false",false,false,false}, {L"no",false,false,false},
        {L" 0 ",false,true,false}, {L" \t\r\n",true,true,true}, {L"1",true,true,true},
        {L"\x4e2d" L"off",true,false,true}};
    for (const auto& c : cases) {
        check(::SetEnvironmentVariableW(L"RLPLAYER_NATIVE_TAGS", c.value) != 0, "set native switch");
        check(::SetEnvironmentVariableW(L"RLPLAYER_TAG_GUESS", c.value) != 0, "set guess switch");
        check(::SetEnvironmentVariableW(L"RLPLAYER_TAG_FILL", c.value) != 0, "set fill switch");
        check(::SetEnvironmentVariableW(L"RLPLAYER_TAG_ONLINE", c.value) != 0, "set online switch");
        check(native::enabled() == c.nativeOn, "native switch behavior");
        check(native::guess_enabled() == c.guessOn, "guess switch behavior");
        check(native::fill_enabled() == c.fillOn && native::online_enabled() == c.fillOn, "trimmed fill/online switch behavior");
    }
    for (size_t length : {size_t(63), size_t(64), size_t(127)}) {
        const std::wstring value = L"off" + std::wstring(length - 3, L' ');
        ::SetEnvironmentVariableW(L"RLPLAYER_NATIVE_TAGS", value.c_str());
        ::SetEnvironmentVariableW(L"RLPLAYER_TAG_GUESS", value.c_str());
        ::SetEnvironmentVariableW(L"RLPLAYER_TAG_FILL", value.c_str());
        check(native::enabled() == (length >= 64), "native bounded buffer boundary");
        check(native::guess_enabled(), "guess does not trim/accept oversized values");
        check(native::fill_enabled() == (length >= 64), "fill bounded buffer boundary");
    }
    const auto dir = std::filesystem::temp_directory_path() / (L"rta_meta_platform_" + std::to_wstring(::GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directory(dir, ec);
    const auto path = dir / L"\xfffd.bin";
    { std::ofstream file(path, std::ios::binary); file << 'x'; }
    const std::string badPath = cue_path_utf8(dir) + "\\\xff.bin";
    {
        native::detail::FileReader reader;
        check(reader.open(badPath), "file path preserves malformed UTF8 replacement");
        native::detail::FileReader nulReader;
        check(nulReader.open(cue_path_utf8(path) + std::string("\0unused", 7)), "file path preserves NUL truncation");
    }
    check(std::filesystem::remove(path, ec), "remove temporary fixture");
    check(std::filesystem::remove(dir, ec), "remove temporary directory");
    std::printf("meta platform: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
