// Real Windows process/lease checks. The child is a synthetic idle process, never a player.
#include "net/update_process.h"
#include <iostream>
#include <stdexcept>
#include <vector>
#include <memory>
using namespace rta::update;
static int checks=0;
static void check(bool ok) {++checks;if(!ok)throw std::runtime_error("process check "+std::to_string(checks));}
static std::wstring wide(const char* s) {std::string v(s);return std::wstring(v.begin(),v.end());}
int main(int argc,char**argv) {
 try {
    if(argc==5) {
        HANDLE ready=OpenEventW(EVENT_MODIFY_STATE,FALSE,wide(argv[3]).c_str());
        HANDLE stop=OpenEventW(SYNCHRONIZE,FALSE,wide(argv[4]).c_str());
        check(ready&&stop);
        std::unique_ptr<InstallRuntimeGuard> guard;
        if(std::string(argv[1])=="--lease") {guard=std::make_unique<InstallRuntimeGuard>(argv[2],false);check(guard->ok());}
        SetEvent(ready);check(WaitForSingleObject(stop,10000)==WAIT_OBJECT_0);
        CloseHandle(stop);CloseHandle(ready);return 0;
    }
    check(argc==2);
    auto root=std::filesystem::u8path(argv[1]) / ("run-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    std::filesystem::create_directories(root);
    const auto root8=root.u8string();std::string install((const char*)root8.data(),root8.size());
    wchar_t self[MAX_PATH];check(GetModuleFileNameW(nullptr,self,MAX_PATH)>0);
    auto spawn=[&](const std::filesystem::path& exe,const char* mode,bool legacy) {
        auto suffix=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(checks);
        auto readyName=L"Local\\RLPlayerAuditReady-"+suffix,stopName=L"Local\\RLPlayerAuditStop-"+suffix;
        HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,readyName.c_str());HANDLE stop=CreateEventW(nullptr,TRUE,FALSE,stopName.c_str());
        check(ready&&stop);
        std::wstring command=L"\""+exe.wstring()+L"\" "+wide(mode)+L" \""+root.wstring()+L"\" "+readyName+L" "+stopName;
        std::vector<wchar_t> buffer(command.begin(),command.end());buffer.push_back(0);
        STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
        check(CreateProcessW(exe.c_str(),buffer.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi));
        check(WaitForSingleObject(ready,5000)==WAIT_OBJECT_0);
        if(legacy) {std::string error;check(install_has_running_server(install,error));check(!error.empty());}
        else {InstallRuntimeGuard exclusive(install,true);check(!exclusive.ok());InstallRuntimeGuard shared(install,false);check(shared.ok());}
        SetEvent(stop);check(WaitForSingleObject(pi.hProcess,5000)==WAIT_OBJECT_0);
        DWORD code=99;check(GetExitCodeProcess(pi.hProcess,&code)&&code==0);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);CloseHandle(ready);CloseHandle(stop);
    };
    spawn(self,"--lease",false);
    {InstallRuntimeGuard exclusive(install,true);check(exclusive.ok());}
    const auto legacy=root/L"rtaweb.exe";std::filesystem::copy_file(self,legacy);
    spawn(legacy,"--idle",true);
    std::string error;check(!install_has_running_server(install,error));
    std::cout<<"[PASS] Windows process/lease checks="<<checks<<" devices=0\n";return 0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}
}
