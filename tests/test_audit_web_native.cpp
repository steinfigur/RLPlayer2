#ifdef NDEBUG
#undef NDEBUG
#endif
#ifndef _WIN32
#error This test exercises real Windows WinSock.
#endif
#include "net/webui.h"
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <iostream>
#pragma comment(lib, "ws2_32.lib")
using namespace rta::net;
static SOCKET connect_to(uint16_t port) {
 SOCKET c=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);assert(c!=INVALID_SOCKET);
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 assert(::connect(c,(sockaddr*)&a,sizeof(a))==0);return c;
}
static void bounded_stop(WebUi&ui) {
 const auto t=std::chrono::steady_clock::now();ui.stop();
 assert(std::chrono::steady_clock::now()-t<std::chrono::seconds(2));
}
int main() {
 WSADATA data{};assert(::WSAStartup(MAKEWORD(2,2),&data)==0);
 for(int i=0;i<3;++i){WebUi ui;assert(ui.start(0,[](const auto&,auto&){return "{}";}));bounded_stop(ui);ui.stop();}
 {WebUi ui;assert(ui.start(0,[](const auto&,auto&){return "{}";}));
  auto c=connect_to(ui.port());const char*partial="GET / HTTP/1.1\r\nHost: localhost\r\n";
  assert(::send(c,partial,(int)std::strlen(partial),0)>0);bounded_stop(ui);::closesocket(c);}
 {WebUi ui;std::mutex gate;std::condition_variable cv;bool entered=false,release=false;
  assert(ui.start(0,[&](const auto&,auto&){std::unique_lock<std::mutex>l(gate);entered=true;cv.notify_all();cv.wait(l,[&]{return release;});return "{}";}));
  auto c=connect_to(ui.port());const char*request="GET / HTTP/1.1\r\nHost: localhost\r\n\r\n";assert(::send(c,request,(int)std::strlen(request),0)>0);
  {std::unique_lock<std::mutex>l(gate);assert(cv.wait_for(l,std::chrono::seconds(3),[&]{return entered;}));}
  std::thread owner([&]{bounded_stop(ui);});{std::lock_guard<std::mutex>l(gate);release=true;cv.notify_all();}owner.join();::closesocket(c);}
 assert(::WSACleanup()==0);
 std::cout<<"PASS 5 real Windows WinSock idle/partial-request/active-handler stop cases\n";
}
