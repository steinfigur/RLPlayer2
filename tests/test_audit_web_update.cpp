#ifdef NDEBUG
#undef NDEBUG // The regression must execute all operations in Release builds as well.
#endif
// Windows filesystem integration: production staging/apply headers, with only OS
// move/copy failures injected. All artifacts live under argv[1], never an install.
#ifndef _WIN32
#error This integration test requires Windows; test_audit_web_staging is portable.
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <condition_variable>
#include <iostream>
#include <string>
static std::function<bool(const std::wstring&,const std::wstring&)> reject_move;
static BOOL WINAPI audit_move(LPCWSTR from,LPCWSTR to,DWORD flags) {
 if(reject_move && reject_move(from,to)){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
 return ::MoveFileExW(from,to,flags);
}
static BOOL WINAPI audit_copy(LPCWSTR from,LPCWSTR to,BOOL failExists) {
 if(reject_move && reject_move(from,to)){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
 return ::CopyFileW(from,to,failExists);
}
#define MoveFileExW audit_move
#define CopyFileW audit_copy
#include "net/update_apply.h"
#undef MoveFileExW
#undef CopyFileW
using namespace rta::update;
namespace fs=std::filesystem;
static std::string base;
static int caseNo=0;
static std::string stage;
static ManifestExt manifest;
static std::string read(const std::string& p){return read_text_file(p);}
static void put(const std::string&p,const std::string&v){assert(ensure_dir(fs::path(p).parent_path().string()));assert(write_text_file(p,v));}
static void reset_state(){auto&s=state();std::lock_guard<std::mutex>l(s.mtx);s.staged=false;s.phase="idle";s.stagingDir.clear();s.dlVersion.clear();}
static void setup(bool added=false,bool reused=true) {
 reject_move={};reset_state();
 const std::string root=base+"\\case-"+std::to_string(++caseNo);
 assert(!path_exists(root));assert(ensure_dir(root));assert(_putenv_s("RLPLAYER_UPDATE_DIR",root.c_str())==0);
 stage=join_path(version_dir("9.8.7"),"staging");assert(ensure_dir(stage));
 manifest={};manifest.ok=true;manifest.version="9.8.7";
 for(const char* name:{"a.dll","b.dll","rlplayer-settings.conf"}) {
  const std::string value=std::string("new-")+name;
  manifest.files.push_back({name,sha256_hex(value),(long long)value.size()});
  put(join_path(stage,name),value);put(join_path(root,name),std::string("old-")+name);
 }
 if(added){std::string v="new-c";manifest.files.push_back({"c.dat",sha256_hex(v),(long long)v.size()});put(join_path(stage,"c.dat"),v);}
 if(reused){std::string v="keep";manifest.files.push_back({"reuse.dat",sha256_hex(v),(long long)v.size()});put(join_path(root,"reuse.dat"),v);}
 std::vector<std::string> downloads={"a.dll","b.dll","rlplayer-settings.conf"};if(added)downloads.push_back("c.dat");
 std::string err;assert(complete_staging(manifest,downloads,stage,err));assert(write_ready_plan("9.8.7","1.0.0",stage,err));
}
static bool contains(const std::wstring& s,const wchar_t* q){return s.find(q)!=std::wstring::npos;}
static void originals(){for(const char*name:{"a.dll","b.dll","rlplayer-settings.conf"})assert(read(join_path(install_dir(),name))==std::string("old-")+name);}
static bool final_journal(const std::wstring&from,const std::wstring&to){
 return contains(to,L"pending.txt") && read(rta::platform::detail::narrow(from)).find("phase=applied\r\n")==0;
}
int main(int argc,char**argv){
 assert(argc==2);base=std::string(argv[1])+"\\run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64());
 assert(base.find("review-fixes")!=std::string::npos || base.find("audit-fixtures")!=std::string::npos);assert(!path_exists(base));assert(ensure_dir(base));
 int checks=0;std::string err;StagingManifest checked;
 setup();::DeleteFileW(rta::platform::from_utf8(staging_complete_path("9.8.7")).c_str());
 restore_staging_state();assert(!state().staged);assert(!write_ready_plan("9.8.7","1",stage,err));++checks;
 setup();::DeleteFileW(rta::platform::from_utf8(join_path(stage,"b.dll")).c_str());
 restore_staging_state();assert(!state().staged);assert(!verify_staging("9.8.7",stage,checked,err));
 assert(apply_pending_on_startup().find("未替换文件")!=std::string::npos);originals();++checks;
 setup();::DeleteFileW(rta::platform::from_utf8(staging_complete_path("9.8.7")).c_str());
 ::DeleteFileW(rta::platform::from_utf8(join_path(stage,"b.dll")).c_str());
 assert(!complete_staging(manifest,{"a.dll","b.dll","rlplayer-settings.conf"},stage,err));
 assert(!path_exists(staging_complete_path("9.8.7")));restore_staging_state();assert(!state().staged);
 put(join_path(stage,"b.dll"),"new-b.dll");
 assert(complete_staging(manifest,{"a.dll","b.dll","rlplayer-settings.conf"},stage,err));
 restore_staging_state();assert(state().staged);++checks;
 setup();put(join_path(stage,"b.dll"),"tampered");assert(!verify_staging("9.8.7",stage,checked,err));++checks;
 setup();put(staging_complete_path("9.8.7"),std::string(64,'0'));assert(!verify_staging("9.8.7",stage,checked,err));++checks;
 setup();put(join_path(install_dir(),"reuse.dat"),"drift");assert(!verify_staging("9.8.7",stage,checked,err));++checks;
 setup();restore_staging_state();assert(state().staged && state().phase=="ready");
 assert(apply_pending_on_startup().find("已替换")!=std::string::npos);assert(read_pending().phase=="applied");
 assert(read(join_path(install_dir(),"a.dll"))=="new-a.dll");assert(read(join_path(install_dir(),"rlplayer-settings.conf"))=="old-rlplayer-settings.conf");
 assert(rollback_pending("synthetic restart failure").find("已回滚")!=std::string::npos);originals();++checks;
 setup(false,false);assert(verify_staging("9.8.7",stage,checked,err));assert(apply_pending_on_startup().find("已替换")!=std::string::npos);++checks;
 // Ready intent is bound to the exact full manifest, even for the same version/file list.
 setup();put(join_path(stage,"a.dll"),"another-a");manifest.files[0].sha256=sha256_hex("another-a");manifest.files[0].size=9;
 assert(complete_staging(manifest,{"a.dll","b.dll","rlplayer-settings.conf"},stage,err));
 assert(apply_pending_on_startup().find("身份已改变")!=std::string::npos);originals();++checks;
 // A concurrent artifact writer makes ready-plan creation explicitly busy.
 setup();std::mutex gate;std::condition_variable cv;bool locked=false,release=false;
 std::thread writer([&]{std::lock_guard<std::recursive_mutex> a(update_artifacts_mutex());
   std::unique_lock<std::mutex> l(gate);locked=true;cv.notify_all();cv.wait(l,[&]{return release;});});
 {std::unique_lock<std::mutex> l(gate);cv.wait(l,[&]{return locked;});}
 assert(!write_ready_plan("9.8.7","1",stage,err));assert(err.find("稍后重试")!=std::string::npos);
 {std::lock_guard<std::mutex> l(gate);release=true;cv.notify_all();}writer.join();++checks;
 // Fail before the second backup, after the first file has been installed.
 setup();reject_move=[](const auto&from,const auto&to){return contains(from,L"b.dll") && contains(to,L"backup");};
 assert(apply_pending_on_startup().find("已回滚")!=std::string::npos);reject_move={};originals();++checks;
 // The original bug: old b.dll is backed up, but installing its replacement fails.
 setup();reject_move=[](const auto&from,const auto&){return contains(from,L"staging\\b.dll");};
 assert(apply_pending_on_startup().find("已回滚")!=std::string::npos);reject_move={};originals();assert(read_pending().phase=="rolled_back");++checks;
 // Failure after all installations must restore old files and remove new files.
 setup(true);reject_move=final_journal;
 assert(apply_pending_on_startup().find("已回滚")!=std::string::npos);reject_move={};originals();assert(!path_exists(join_path(install_dir(),"c.dat")));++checks;
 // Rollback failure is reported, with the old contents preserved in backup.
 setup();reject_move=[](const auto&from,const auto&){return contains(from,L"staging\\b.dll") || contains(from,L"backup\\b.dll");};
 assert(apply_pending_on_startup().find("回滚未完成")!=std::string::npos);assert(read_pending().phase=="rollback_failed");
 assert(read(join_path(backup_dir("9.8.7"),"b.dll"))=="old-b.dll");reject_move={};
 assert(apply_pending_on_startup().find("已回滚")!=std::string::npos);originals();++checks;
 // Crash boundary: write-ahead intent exists and the old file was already moved.
 setup();auto p=read_pending();p.phase="applying";p.applied={"a.dll"};p.originalSha["a.dll"]=sha256_hex("old-a.dll");assert(write_pending(p));
 assert(ensure_dir(backup_dir(p.version)));assert(move_file(join_path(install_dir(),"a.dll"),join_path(backup_dir(p.version),"a.dll")));
 assert(apply_pending_on_startup().find("已回滚")!=std::string::npos);originals();++checks;
 // A final-journal write fails AND rollback fails: preserve the durable applying plan.
 setup();reject_move=[](const auto&from,const auto&to){return final_journal(from,to) || contains(from,L"backup\\a.dll");};
 assert(apply_pending_on_startup().find("回滚未完成")!=std::string::npos);reject_move={};assert(read_pending().phase=="rollback_failed");
 assert(read(join_path(backup_dir("9.8.7"),"a.dll"))=="old-a.dll");++checks;
 // An absent backup with a new target is not proof of a completed rollback.
 setup();auto missing=read_pending();missing.phase="applying";missing.applied={"a.dll"};
 missing.originalSha["a.dll"]=sha256_hex("old-a.dll");assert(write_pending(missing));
 put(join_path(install_dir(),"a.dll"),"new-a.dll");
 assert(apply_pending_on_startup().find("回滚失败")!=std::string::npos);
 assert(read_pending().phase=="rollback_failed");++checks;
 // An independently opened share=0 handle excludes every writer entry point.
 setup();const auto lockpath=rta::platform::from_utf8(join_path(updates_root(),"writer.lock"));
 HANDLE external=::CreateFileW(lockpath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
 assert(external!=INVALID_HANDLE_VALUE);
 assert(!write_ready_plan("9.8.7","1",stage,err));assert(err.find("另一进程")!=std::string::npos);
 assert(apply_pending_on_startup().find("未执行")!=std::string::npos);originals();
 assert(rollback_pending("blocked").find("未执行")!=std::string::npos);
 assert(confirm_on_startup().find("未执行")!=std::string::npos);
 assert(!start_download(err));download_worker(manifest,Plan{});assert(state().phase=="error");
 assert(::CloseHandle(external));++checks;
 // Nested same-thread scopes share the lease; exception unwinding releases it.
 try {UpdateWriterLease outer(err);assert(outer.owns_lock());
   {UpdateWriterLease inner(err);assert(inner.owns_lock());}
   HANDLE second=::CreateFileW(lockpath.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
   assert(second==INVALID_HANDLE_VALUE && GetLastError()==ERROR_SHARING_VIOLATION);
   throw std::runtime_error("synthetic lease unwind");
 } catch(const std::runtime_error&) {}
 external=::CreateFileW(lockpath.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
 assert(external!=INVALID_HANDLE_VALUE);assert(::CloseHandle(external));++checks;
 std::cout<<"PASS "<<checks<<" production Windows staging/apply/rollback cases; artifacts="<<base<<"\n";
}
