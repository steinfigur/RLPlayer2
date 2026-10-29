#pragma once
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
using SOCKET = int; using DWORD = unsigned long;
struct WSADATA {};
struct sockaddr {};
struct sockaddr_in { int sin_family; unsigned short sin_port; struct { unsigned long s_addr; } sin_addr; };
constexpr int INVALID_SOCKET=-1, SOCKET_ERROR=-1, AF_INET=2, SOCK_STREAM=1, IPPROTO_TCP=6,
 SOL_SOCKET=1, SO_REUSEADDR=2, SO_RCVTIMEO=3, SO_SNDTIMEO=4, SD_BOTH=2,
 MSG_PARTIAL=0x8000, WSAEOPNOTSUPP=1, MAX_COMPUTERNAME_LENGTH=63;
constexpr unsigned long INADDR_LOOPBACK=0x7f000001, INADDR_ANY=0;
#define MAKEWORD(a,b) 0
namespace audit_net {
struct Client { std::string input, output; bool blockRecv=false, blockSend=false, recvEntered=false,
 sendEntered=false, shut=false, closed=false; };
inline std::mutex mx;
inline std::condition_variable cv;
inline std::map<SOCKET, std::shared_ptr<Client>> clients;
inline std::deque<SOCKET> incoming;
inline bool listenerStopped=true;
inline int next=100, startups=0, cleanups=0;
inline bool failSocket=false,failBind=false,failListen=false;
inline SOCKET add(std::string input, bool blockRecv=false, bool blockSend=false) {
 std::lock_guard<std::mutex> lock(mx);
 SOCKET id=++next; auto c=std::make_shared<Client>(); c->input=std::move(input);
 c->blockRecv=blockRecv; c->blockSend=blockSend; clients[id]=c; incoming.push_back(id); cv.notify_all(); return id;
}
template<class F> inline void wait(F f) {
 std::unique_lock<std::mutex> lock(mx);
 if (!cv.wait_for(lock, std::chrono::seconds(5), f)) throw std::runtime_error("mock barrier timed out");
}
}
inline unsigned short htons(unsigned short p){return p;}
inline unsigned short ntohs(unsigned short p){return p;}
inline unsigned long htonl(unsigned long p){return p;}
inline int WSAStartup(int,WSADATA*){++audit_net::startups; return 0;}
inline int WSACleanup(){++audit_net::cleanups; return 0;}
inline int WSAGetLastError(){return 0;}
inline SOCKET socket(int,int,int){std::lock_guard<std::mutex> l(audit_net::mx); audit_net::listenerStopped=false; return audit_net::failSocket ? INVALID_SOCKET : 10;}
inline int setsockopt(SOCKET,int,int,const char*,int){return 0;}
inline int bind(SOCKET,sockaddr*,int){return audit_net::failBind ? -1 : 0;}
inline int listen(SOCKET,int){return audit_net::failListen ? -1 : 0;}
inline int getsockname(SOCKET,sockaddr* p,int*){reinterpret_cast<sockaddr_in*>(p)->sin_port=8899; return 0;}
inline int shutdown(SOCKET s,int){std::lock_guard<std::mutex> l(audit_net::mx);
 if(s==10) audit_net::listenerStopped=true; else audit_net::clients.at(s)->shut=true;
 audit_net::cv.notify_all(); return 0;}
inline int closesocket(SOCKET s){std::lock_guard<std::mutex> l(audit_net::mx);
 if(s!=10) audit_net::clients.at(s)->closed=true; audit_net::cv.notify_all(); return 0;}
inline bool GetComputerNameA(char*p,DWORD*n){std::strcpy(p,"mock");*n=4;return true;}
inline SOCKET accept(SOCKET,void*,void*){std::unique_lock<std::mutex> l(audit_net::mx);
 audit_net::cv.wait(l,[]{return audit_net::listenerStopped || !audit_net::incoming.empty();});
 if(audit_net::listenerStopped) return INVALID_SOCKET;
 auto s=audit_net::incoming.front(); audit_net::incoming.pop_front(); audit_net::cv.notify_all(); return s;}
inline int recv(SOCKET s,char*p,int n,int){std::unique_lock<std::mutex> l(audit_net::mx);auto c=audit_net::clients.at(s);
 c->recvEntered=true; audit_net::cv.notify_all(); audit_net::cv.wait(l,[&]{return !c->blockRecv || c->shut;});
 if(c->shut) return -1; n=std::min(n,(int)c->input.size()); std::memcpy(p,c->input.data(),n);c->input.erase(0,n);return n;}
inline int send(SOCKET s,const char*p,int n,int){std::unique_lock<std::mutex> l(audit_net::mx);auto c=audit_net::clients.at(s);
 c->sendEntered=true;audit_net::cv.notify_all();audit_net::cv.wait(l,[&]{return !c->blockSend || c->shut;});
 if(c->shut) return -1;c->output.append(p,n);return n;}
