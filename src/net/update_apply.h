// src/net/update_apply.h — 在线更新（Windows 版）第二层：受保护清单 / 落地 / 回滚 / 台账
//
// ============================ 为什么这样替换 ============================
// 核心约束：
//   1) 程序正在运行，**不能覆盖自己**：被映射的 exe/dll 不允许删除或覆盖（ERROR_ACCESS_DENIED）。
//      但 Windows **允许改名**（同卷 MoveFileEx）—— 这是自我替换的通用技巧：
//          MoveFileEx(RLPlayer.exe -> updates/<ver>/backup/RLPlayer.exe)   成功（进程还在跑）
//          MoveFileEx(staging/RLPlayer.exe -> RLPlayer.exe)                成功（名字腾出来了）
//      下次启动就是新版；旧映像仍在内存里跑完这一轮，不影响。
//   2) 更**不能丢用户数据**：受保护清单里的文件一个字节都不动（见 protected_files/dirs）。
//   3) 失败必须能回滚，且回滚材料（backup）必须**在替换之前**就已经落盘。
//
// 流程（用户原话：「完全可以做到自动更新，然后重启或者刷新，就可以完成」）：
//   · rtaweb 端点 /api/update/apply —— 只做三件事：把 staging + 计划写进 updates/pending.txt、
//     返回"即将重启"、然后请自己与启动器退出（**不在这里换文件**，此刻文件都在用）。
//   · 下次启动 RLPlayer.exe（启动器）—— main() 一开始、**在起 rtaweb 之前**调用
//     apply_pending_on_startup()：此刻 rtaweb 没在跑，所有文件都不被占用；逐个
//     "旧文件改名进 backup → 新文件改名到位"，连它自己（RLPlayer.exe）也是这样换掉。
//   · 新 rtaweb 起来后调用 confirm_on_startup()：版本对上 ⟹ 台账写 done、清 staging。
//   · 起不来（launcher 的端口等待窗口内子进程退出）⟹ rollback_pending() 把 backup 搬回去。
//
// 为什么不再用"生成 update.ps1 再拉起 PowerShell"（我最初的实现）：
//   ① 依赖 powershell.exe 与 ExecutionPolicy（企业 AllSigned 会直接失败）；
//   ② 多一个进程就多一个失败面（脚本被杀/被杀软拦），且它还得轮询 PID 等我们退出；
//   ③ 用户的语义是「重启就完成」，启动器启动时本来就有"文件不被占用"的完美时机。
//   ⟹ 改成启动器内直接落地，链路更短、无外部依赖、可被同一份单测覆盖。
#pragma once

#include <algorithm>
#include <string>
#include <vector>

#include "net/update.h"
#include "net/update_process.h"

namespace rta::update {

// ===========================================================================
// 1. 受保护清单（纯函数，可离线断言；脚本/代码/UI 共用同一份语义）
// ===========================================================================
// 判据来源：需求原文「要保护的（绝对不能被覆盖）」+ 本仓实际落盘约定。
inline const std::vector<std::string>& protected_files() {
    static const std::vector<std::string> v = {
        // 需求点名的 7 个
        "rlplayer-settings.conf",       // 设置（后端/滤波/抖动/音量…）
        "rlplayer-library.conf",        // 音乐库根目录
        "rlplayer-llm.conf",            // DeepSeek key / 代理
        "rlplayer-feedback.conf",       // 一键反馈的令牌/仓库
        "rlplayer-queue.txt",           // 当前播放队列
        "rlplayer-savedqueues.conf",    // 具名保存的队列
        "rlplayer-favs.json",           // 收藏
        // 同族：用户数据 / 运行期产物
        "rlplayer-sources.conf", "rlplayer-peq.txt", "rlplayer-index.tsv",
        "rlplayer-recent.txt", "rlplayer-diag.json", "rlplayer-playback.log",
        "launcher.log", "rlplayer-llm-usage.json", "rlplayer-update.conf",
    };
    return v;
}

// 整目录保护（data 里的纯应用资产另见 protected_allow()）
inline const std::vector<std::string>& protected_dirs() {
    static const std::vector<std::string> v = {
        "data",            // 库索引 / 封面缓存 / 房间校正集
        "covercache",
        "webview2runtime", // 127 MB 运行时，几乎不变
        "webview2",
        "playlists",
        "rlplayer-meta",
        "updates",         // 我们自己的工作目录
        "dumps",
        "feedback-outbox",
    };
    return v;
}

// 保护目录里的**例外**：随包发布的纯应用资产（不更新会与新代码不匹配）
inline const std::vector<std::string>& protected_allow() {
    static const std::vector<std::string> v = { "data\\opra_index.json" };
    return v;
}

inline std::string norm_rel2(std::string rel) {
    for (char& c : rel) {
        if (c == '/') c = '\\';
        else if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    }
    while (!rel.empty() && (rel.front() == '\\' || rel.front() == '.')) rel.erase(rel.begin());
    return rel;
}

inline bool starts_with_dir(const std::string& rel, const std::string& dir) {
    if (rel.size() < dir.size()) return false;
    if (rel.compare(0, dir.size(), dir) != 0) return false;
    return rel.size() == dir.size() || rel[dir.size()] == '\\';
}

// 相对路径是否「绝对不能碰」
inline bool is_protected_rel(const std::string& rawRel) {
    const std::string rel = norm_rel2(rawRel);
    if (rel.empty()) return true;
    for (const auto& a : protected_allow()) if (rel == a) return false;
    for (const auto& d : protected_dirs()) if (starts_with_dir(rel, d)) return true;
    const size_t s = rel.find_last_of('\\');
    const std::string leaf = (s == std::string::npos) ? rel : rel.substr(s + 1);
    for (const auto& f : protected_files()) if (rel == f || leaf == f) return true;
    return false;
}

// ===========================================================================
// 2. 台账 updates/pending.txt
// ===========================================================================
inline std::string pending_path() { return join_path(updates_root(), "pending.txt"); }

struct PendingPlan {
    bool exists = false;
    std::string phase;      // ready / applied / rolled_back / apply_failed / done
    std::string version;
    std::string from;
    std::string staging;
    std::string time;
    std::string detail;
    std::string manifestSha; // Binds ready intent to the exact completed full manifest.
    std::map<std::string, std::string> originalSha; // Existing bytes, persisted before any move.
    std::vector<std::string> files;    // 计划安装的文件（相对路径）
    std::vector<std::string> applied;  // 实际替换成功的（回滚用）
    std::vector<std::string> added;    // 本来不存在、本次新增的（回滚时删掉）
};

inline PendingPlan read_pending() {
    PendingPlan p;
    if (!path_exists(pending_path())) return p;
    p.exists = true;
    const std::string t = read_text_file(pending_path());
    size_t pos = 0;
    while (pos < t.size()) {
        size_t e = t.find('\n', pos);
        if (e == std::string::npos) e = t.size();
        std::string line = t.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "phase") p.phase = v;
        else if (k == "version") p.version = v;
        else if (k == "from") p.from = v;
        else if (k == "staging") p.staging = v;
        else if (k == "time") p.time = v;
        else if (k == "detail") p.detail = v;
        else if (k == "manifestSha") p.manifestSha = v;
        else if (k == "old_sha") {
            const size_t sep = v.find('\t');
            if (sep != std::string::npos) p.originalSha[v.substr(0, sep)] = v.substr(sep + 1);
        }
        else if (k == "file") p.files.push_back(v);
        else if (k == "applied") p.applied.push_back(v);
        else if (k == "added") p.added.push_back(v);
    }
    return p;
}

inline bool write_pending(const PendingPlan& p) {
    ensure_dir(updates_root());
    std::string t = "phase=" + p.phase + "\r\n";
    t += "version=" + p.version + "\r\n";
    t += "from=" + p.from + "\r\n";
    t += "staging=" + p.staging + "\r\n";
    t += "time=" + p.time + "\r\n";
    if (!p.detail.empty()) t += "detail=" + p.detail + "\r\n";
    if (!p.manifestSha.empty()) t += "manifestSha=" + p.manifestSha + "\r\n";
    for (const auto& e : p.originalSha) t += "old_sha=" + e.first + "\t" + e.second + "\r\n";
    for (const auto& f : p.files) t += "file=" + f + "\r\n";
    for (const auto& f : p.applied) t += "applied=" + f + "\r\n";
    for (const auto& f : p.added) t += "added=" + f + "\r\n";
    return write_update_atomic(pending_path(), t);
}

inline std::string now_text() {
    char b[64];
    std::time_t t = std::time(nullptr);
    const std::tm v = rta::platform::update_local_time(t);
    std::snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
                  v.tm_year + 1900, v.tm_mon + 1, v.tm_mday, v.tm_hour, v.tm_min, v.tm_sec);
    return std::string(b);
}

inline std::string backup_dir(const std::string& ver) { return join_path(version_dir(ver), "backup"); }

// 把 staging 里的文件清单写进台账（端点 /apply 调用）
inline bool write_ready_plan(const std::string& ver, const std::string& from,
                             const std::string& staging, std::string& err) {
    UpdateWriterLease writer(err);
    if (!writer.owns_lock()) return false;
    { auto& s = state(); std::lock_guard<std::mutex> lock(s.mtx);
      if (s.downloading) { err = "更新尚未下载完成"; return false; } }
    StagingManifest manifest;
    if (!verify_staging(ver, staging, manifest, err)) return false;
    const PendingPlan previous = read_pending();
    if (previous.exists && (previous.phase == "applying" || previous.phase == "applied" ||
        previous.phase == "rollback_failed")) { err = "已有未结束的更新事务，保留备份和台账"; return false; }
    std::vector<std::string> rels;
    for (const auto& e : manifest.files) if (e.staged) rels.push_back(e.file.path);
    PendingPlan p;
    p.phase = "ready";
    p.manifestSha = sha256_hex(read_text_file(staging_manifest_path(ver)));
    p.version = ver;
    p.from = from;
    p.staging = staging;
    p.time = now_text();
    for (const auto& r : rels) p.files.push_back(r);
    if (!write_pending(p)) { err = "无法写台账：" + pending_path(); return false; }
    return true;
}

// 同卷改名优先；跨卷（极端情况）退回 复制+删除
inline bool move_file(const std::string& from, const std::string& to) {
    const auto wf = rta::platform::from_utf8(from);
    const auto wt = rta::platform::from_utf8(to);
    rta::platform::reset_file_attributes(wt);
    if (rta::platform::move_file(wf, wt, true)) return true;
    if (!rta::platform::copy_file(wf, wt, false)) return false;
    rta::platform::reset_file_attributes(wf);
    return rta::platform::remove_file(wf);
}

// Rollback uses the journal written BEFORE touching each file. A crash after moving
// the old file but before installing the new one therefore still has a recovery entry.
inline bool rollback_files(PendingPlan& p, std::string& detail) {
    if (!parse_version(p.version).valid || p.version.find_first_of("/\\\r\n") != std::string::npos ||
        p.staging != join_path(version_dir(p.version), "staging")) { detail += "；事务路径非法"; return false; }
    std::set<std::string> seen;
    for (const auto& rel : p.applied) {
        if (!safe_install_rel(rel) || is_protected_rel(rel) || !seen.insert(lower_ascii(norm_rel(rel))).second ||
            std::find(p.files.begin(), p.files.end(), rel) == p.files.end()) { detail += "；事务文件记录非法"; return false; }
    }
    for (const auto& rel : p.added) if (std::find(p.applied.begin(), p.applied.end(), rel) == p.applied.end()) {
        detail += "；新增文件事务记录非法"; return false;
    }
    const std::string inst = install_dir(), bak = backup_dir(p.version);
    std::vector<std::string> unresolved;
    for (auto it = p.applied.rbegin(); it != p.applied.rend(); ++it) {
        const std::string tgt = join_path(inst, *it), b = join_path(bak, *it);
        const bool added = std::find(p.added.begin(), p.added.end(), *it) != p.added.end();
        bool ok = false;
        if (added) ok = !path_exists(tgt) || rta::platform::remove_file(rta::platform::from_utf8(tgt)) != 0;
        else {
            const auto original = p.originalSha.find(*it);
            auto matches_original = [&](const std::string& path) {
                std::string hex, error;
                return original != p.originalSha.end() && sha256_file(path, hex, error) &&
                    hex_equals_ci(hex, original->second);
            };
            if (path_exists(b)) {
                // Legacy journals can still restore an extant backup; new journals
                // verify its bytes before consuming the recovery material.
                ok = (original == p.originalSha.end() || matches_original(b)) && move_file(b, tgt);
            } else ok = matches_original(tgt); // prove old bytes, never infer rollback from existence
        }
        if (!ok) unresolved.push_back(*it);
    }
    if (!unresolved.empty()) {
        detail += "；回滚未完成，保留备份/台账：";
        for (const auto& rel : unresolved) detail += rel + " ";
        return false;
    }
    return true;
}
inline std::string rollback_pending(const std::string& why) {
    std::string leaseError; UpdateWriterLease writer(leaseError);
    if (!writer.owns_lock()) return "UPDATE 未执行：" + leaseError;
    PendingPlan p = read_pending();
    if (!p.exists || (p.phase != "applied" && p.phase != "applying" && p.phase != "rollback_failed")) return {};
    InstallRuntimeGuard runtime(install_dir(), true);
    if (!runtime.ok()) return "UPDATE 恢复暂缓：" + runtime.error();
    std::string runningError;
    if (install_has_running_server(install_dir(), runningError)) return "UPDATE 恢复暂缓：" + runningError;
    std::string detail = why;
    const bool restored = rollback_files(p, detail);
    p.phase = restored ? "rolled_back" : "rollback_failed";
    p.time = now_text(); p.detail = detail;
    if (!write_pending(p)) return "UPDATE 恢复结果无法写入台账（保留原台账与备份）：" + detail;
    return std::string(restored ? "UPDATE 已回滚：" : "UPDATE 回滚失败：") + detail;
}

inline std::string apply_pending_on_startup() {
    std::string leaseError; UpdateWriterLease writer(leaseError);
    if (!writer.owns_lock()) return "UPDATE 未执行：" + leaseError;
    PendingPlan p = read_pending();
    if (!p.exists) return {};
    if (p.phase == "applying" || p.phase == "rollback_failed")
        return rollback_pending("恢复中断的更新事务");
    if (p.phase != "ready") return {};
    auto reject = [&](const std::string& why) {
        p.phase = "apply_failed"; p.detail = why;
        const bool saved = write_pending(p);
        return "UPDATE 失败（未替换文件）：" + why + (saved ? "" : "；无法写入失败台账");
    };
    InstallRuntimeGuard runtime(install_dir(), true);
    if (!runtime.ok()) return "UPDATE 暂缓：" + runtime.error();
    std::string runningError;
    if (install_has_running_server(install_dir(), runningError)) return "UPDATE 暂缓：" + runningError;
    StagingManifest manifest; std::string error;
    if (!verify_staging(p.version, p.staging, manifest, error)) return reject(error);
    if (p.manifestSha != sha256_hex(read_text_file(staging_manifest_path(p.version)))) return reject("安装计划的完整清单身份已改变");
    std::vector<std::string> expected;
    for (const auto& e : manifest.files) if (e.staged) expected.push_back(e.file.path);
    if (p.files != expected) return reject("安装计划与完整清单不一致");
    const std::string inst = install_dir(), bak = backup_dir(p.version);
    // A previous backup is recovery material, never silently overwrite it.
    if (is_dir(bak) && count_files(bak) != 0) return reject("备份目录已有文件，需先处理上次更新材料");
    if (!ensure_dir(bak)) return reject("无法创建备份目录");
    std::vector<std::string> todo;
    for (const auto& rel : expected) if (!is_protected_rel(rel)) todo.push_back(rel);
    if (todo.empty()) return reject("可安装文件为 0");
    p.phase = "applying"; p.applied.clear(); p.added.clear(); p.originalSha.clear();
    if (!write_pending(p)) return "UPDATE 失败：无法持久化事务，未替换文件";
    auto abort = [&](const std::string& why) {
        std::string detail = why;
        const bool restored = rollback_files(p, detail);
        p.phase = restored ? "rolled_back" : "rollback_failed";
        p.detail = detail;
        const bool saved = write_pending(p);
        return std::string(restored ? "UPDATE 失败并已回滚：" : "UPDATE 失败且回滚未完成：") +
            detail + (saved ? "" : "；无法写入恢复结果，保留原事务台账");
    };
    for (const auto& rel : todo) {
        const std::string tgt = join_path(inst, rel), src = join_path(p.staging, rel), b = join_path(bak, rel);
        const size_t sp = tgt.find_last_of("\\/");
        if (sp != std::string::npos && !ensure_dir(tgt.substr(0, sp))) return abort("无法创建安装目录：" + rel);
        const bool existed = path_exists(tgt);
        if (existed) {
            const size_t bs = b.find_last_of("\\/");
            if (bs != std::string::npos && !ensure_dir(b.substr(0, bs))) return abort("无法创建备份目录：" + rel);
        }
        if (existed) {
            std::string hex, hashError;
            if (!sha256_file(tgt, hex, hashError)) return abort("无法记录旧文件摘要：" + rel);
            p.originalSha[rel] = hex;
        }
        // Write-ahead intent covers failures before backup, between backup/install,
        // after install, and even an interrupted write of the next journal update.
        p.applied.push_back(rel);
        if (!existed) p.added.push_back(rel);
        if (!write_pending(p)) return abort("无法记录文件事务：" + rel);
        if (existed && !move_file(tgt, b)) return abort("备份失败：" + rel);
        if (!move_file(src, tgt)) return abort("安装失败：" + rel);
    }
    p.phase = "applied"; p.time = now_text();
    p.detail = "替换 " + std::to_string(todo.size()) + " 个文件";
    if (!write_pending(p)) return abort("无法记录安装完成状态");
    return "UPDATE 已替换 " + std::to_string(todo.size()) + " 个文件，备份在 " + bak;
}

// ===========================================================================
// 5. 确认（新 rtaweb 启动时调用）
// ===========================================================================
inline std::string confirm_on_startup() {
    std::string leaseError; UpdateWriterLease writer(leaseError);
    if (!writer.owns_lock()) return "UPDATE 未执行：" + leaseError;
    PendingPlan p = read_pending();
    if (!p.exists) return std::string();
    if (p.phase == "applied") {
        if (cmp_versions(p.version, current_version()) == 0) {
            p.phase = "done";
            p.time = now_text();
            if (!write_pending(p)) return "UPDATE 确认失败：无法写入完成台账，保留暂存与备份";
            // 成功之后 staging 可以清；backup 保留（用户想手工回退还有料）
            rmtree(join_path(version_dir(p.version), "staging"));
            return "UPDATE ok " + p.from + " -> " + p.version + "（" + p.detail + "）";
        }
        return "UPDATE 台账说装的是 v" + p.version + "，但当前进程是 v" + current_version() +
               " —— 换文件可能没落到本目录";
    }
    if (p.phase == "rolled_back") return "UPDATE 上次更新失败已回滚：" + p.detail;
    if (p.phase == "apply_failed") return "UPDATE 上次更新被拒绝（未替换文件）：" + p.detail;
    if (p.phase == "rollback_failed" || p.phase == "applying") return "UPDATE 更新事务未完成，保留备份等待恢复：" + p.detail;
    if (p.phase == "ready") return "UPDATE 有已下载但尚未安装的更新（重启后生效）";
    return std::string();
}

// ===========================================================================
// 6. 给 /api/update/state 用
// ===========================================================================
inline std::string state_json() {
    const PendingPlan p = read_pending();
    auto& s = state();
    bool staged = false;
    std::string ver, staging;
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        staged = s.staged;
        ver = s.dlVersion;
        staging = s.stagingDir;
    }
    std::string j = "{";
    j += "\"ok\":true";
    j += ",\"current\":\"" + json_escape(current_version()) + "\"";
    j += ",\"installDir\":\"" + json_escape(install_dir()) + "\"";
    j += ",\"skipVersion\":\"" + json_escape(skip_version()) + "\"";
    j += ",\"staged\":" + std::string(staged ? "true" : "false");
    j += ",\"stagedVersion\":\"" + json_escape(ver) + "\"";
    j += ",\"stagingDir\":\"" + json_escape(staging) + "\"";
    j += ",\"pendingExists\":" + std::string(p.exists ? "true" : "false");
    j += ",\"pendingPhase\":\"" + json_escape(p.phase) + "\"";
    j += ",\"pendingVersion\":\"" + json_escape(p.version) + "\"";
    j += ",\"pendingFrom\":\"" + json_escape(p.from) + "\"";
    j += ",\"pendingTime\":\"" + json_escape(p.time) + "\"";
    j += ",\"pendingDetail\":\"" + json_escape(p.detail) + "\"";
    j += ",\"pendingFiles\":" + std::to_string((long long)p.applied.size());
    j += ",\"backupDir\":\"" + json_escape(backup_dir(p.version)) + "\"";
    j += "}";
    return j;
}

// ===========================================================================
// 7. 请启动器退出（"立即重启并更新"用；铁律 2：只发它本来就会处理的 WM_COMMAND）
// ===========================================================================
inline bool request_launcher_exit() {
    return rta::platform::request_update_launcher_exit();
}

inline std::string powershell_unused_marker() { return std::string(); }

} // namespace rta::update
