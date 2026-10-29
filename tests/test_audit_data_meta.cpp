#include "audit_data_fixture.h"
// Actual metadata headers; the platform HTTP transport is a no-network test adapter.
#include <cstring>
#include "meta/retrieval.h"
#include <iostream>
#include <stdexcept>
static int checks = 0;
static void check(bool pass) { ++checks; if (!pass) throw std::runtime_error("meta check " + std::to_string(checks)); }
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::string fixture = audit_fixture(argv[1]);
    using namespace rta::meta;
    const std::string base = fixture;
    fsx::create_directories(meta_path_of(base));
    SummaryResult summary; summary.ok = true; summary.generated_at = std::time(nullptr); summary.expires_at = summary.generated_at + 100000;
    summary.fields = {{"title", "中文😀"}};
    check(summary_save_cache(base, "E", "zh", summary));
    SummaryResult loaded;
    check(summary_load_cache(base, "E", "zh", loaded) && !summary_is_expired(loaded));
    const std::vector<std::string> bad = {"/../../outside", "../en", "..", "en/US", "en\\US", "%2f..", "%252f", "C:\\tmp", "\\\\server\\share", "zh:en", "zh..en"};
    const auto sentinel = meta_path_of(base) / "sentinel";
    entity_write_atomic(sentinel, "SAFE");
    for (const auto& lang : bad) {
        check(!summary_language_valid(lang));
        check(!summary_save_cache(base, "E", lang, summary));
        check(!summary_load_cache(base, "E", lang, loaded));
    }
    for (const auto& lang : {"", "zh", "en", "ja", "zh-CN", "pt-BR"}) check(summary_language_valid(lang));
    std::string sentinelBytes; check(entity_read_file(sentinel, sentinelBytes) && sentinelBytes == "SAFE");
    check(!summary_save_cache(base, "../E", "zh", summary));
    const std::string original = summary_to_json(summary);
    for (int version = 0; version <= 7; ++version) {
        std::string json = original;
        const auto pos = json.find("\"v\": 6");
        if (!version) json.erase(pos, 7); // Missing version (leaves valid next field).
        else json.replace(pos, 6, "\"v\": " + std::to_string(version));
        check(entity_write_atomic(summary_cache_path(base, "V", "zh"), json));
        SummaryResult parsed;
        check(summary_load_cache(base, "V", "zh", parsed) == (version == 5 || version == 6));
        RetrievalOptions opts; opts.metabase = base; opts.entity_id = "V"; opts.offline = true;
        auto result = retrieval_fetch(EntityKind::Person, "Artist", "zh", opts);
        check(result.from_cache == (version == 5 || version == 6));
    }
    for (const auto& ttl : {"null", "\"bad\"", "-1", "0"}) {
        auto json = original; const auto begin = json.find("\"expires_at\": ") + 14; const auto end = json.find(',', begin);
        json.replace(begin, end - begin, ttl);
        SummaryResult parsed; check(summary_from_json(json, parsed));
        // Invalid/nonpositive TTL must expire, never become a fresh paid result.
        check(summary_is_expired(parsed));
    }
    KnowledgeMaterial zh; zh.lang = "zh"; zh.source = "wikipedia-zh"; zh.subject = "Artist"; zh.text = "中文"; zh.url = "https://example.invalid/zh"; zh.fetched_at = std::time(nullptr); zh.expires_at = zh.fetched_at + 100000;
    auto en = zh; en.lang = "en"; en.source = "wikipedia-en"; en.text = "English"; en.url = "https://example.invalid/en";
    check(knowledge_save_cache(base, "K", "Artist", {zh}, "zh"));
    check(knowledge_save_cache(base, "K", "Artist", {en}, "en"));
    KnowledgeOpts opts; opts.metabase = base; opts.include_wikipedia = false; opts.include_moegirl = false;
    for (const auto& lang : {"zh", "en"}) {
        opts.lang = lang; const auto result = knowledge_get("K", "Artist", opts);
        check(result.from_cache && result.materials.size() == 1 && result.materials[0].lang == lang);
    }
    opts.lang = "zh"; opts.refresh = true;
    const auto refreshed = knowledge_get("K", "Artist", opts);
    check(refreshed.from_cache && refreshed.offline); // Refresh bypasses the immediate fresh-cache return.
    opts.refresh = false;
    auto expiredMaterial = en; expiredMaterial.expires_at = (long long)std::time(nullptr) - 1;
    check(knowledge_save_cache(base, "EXPIRED", "Artist", {expiredMaterial}, "en"));
    opts.lang = "en";
    const auto expiredResult = knowledge_get("EXPIRED", "Artist", opts);
    check(expiredResult.from_cache && expiredResult.offline && expiredResult.materials[0].lang == "en");
    check(entity_write_atomic(knowledge_cache_path(base, "WRONG", "zh"), knowledge_to_json({zh}, "OTHER", "Artist", "zh")));
    opts.lang = "zh"; check(!knowledge_get("WRONG", "Artist", opts).from_cache);
    knowledge_mark_miss(base, "MISS", "Artist", "no zh", "zh");
    check(knowledge_recent_miss(base, "MISS", 21600, nullptr, "zh"));
    check(!knowledge_recent_miss(base, "MISS", 21600, nullptr, "en"));
    std::string negativeBytes;
    check(entity_read_file(knowledge_miss_path(base, "MISS", "zh"), negativeBytes));
    check(entity_write_atomic(knowledge_miss_path(base, "MISS", "en"), negativeBytes));
    check(!knowledge_recent_miss(base, "MISS", 21600, nullptr, "en"));
    std::string legacy = knowledge_to_json({zh}, "LEGACY", "Artist");
    const auto lp = legacy.find("  \"language\""); const auto le = legacy.find('\n', lp); legacy.erase(lp, le - lp + 1);
    check(entity_write_atomic(knowledge_entity_path(base, "LEGACY", "knowledge.json"), legacy));
    opts.lang = "en"; check(!knowledge_get("LEGACY", "Artist", opts).from_cache);
    opts.lang = "zh"; check(knowledge_get("LEGACY", "Artist", opts).from_cache);
    // A syntactically truncated MetaStore record is rejected by its actual loading entry point.
    auto& store = MetaStore::instance(); store.setBaseDir(base);
    const auto track = meta_path_of(base) / "tracks" / (store.hashOf("track.wav") + ".json");
    check(entity_write_atomic(track, "{\"path\":\"track.wav\",\"fields\":{\"title\":{\"value\":\"PARTIAL"));
    TrackMeta meta; check(!store.load("track.wav", meta));
    std::cout << "PASS metadata " << checks << " checks\n";
}
