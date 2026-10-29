// Real WinSock request framing boundaries; no application actions or external network.
#include "net/webui.h"
#include <iostream>
#include <stdexcept>
#pragma comment(lib, "ws2_32.lib")
using namespace rta::net;
static int checks=0, failures=0;
static void check(bool ok,const char* label){++checks;if(!ok){++failures;std::cerr<<"[FAIL] "<<label<<'\n';}}
static int request(uint16_t port,const std::string& text,bool halfClose=false,bool splitTerminator=false){
 SOCKET c=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(c==INVALID_SOCKET)throw std::runtime_error("socket");
 DWORD timeout=2000;::setsockopt(c,SOL_SOCKET,SO_RCVTIMEO,(const char*)&timeout,sizeof(timeout));
 sockaddr_in a{};a.sin_family=AF_INET;a.sin_port=htons(port);a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 if(::connect(c,(sockaddr*)&a,sizeof(a))!=0){::closesocket(c);throw std::runtime_error("connect");}
 size_t sent=0;const size_t first=splitTerminator?text.size()-4:text.size();
 while(sent<text.size()){
  if(splitTerminator&&sent>=first)::Sleep(15);
  const size_t count=sent<first?first-sent:1;
  int n=::send(c,text.data()+sent,(int)count,0);if(n<=0)break;sent+=(size_t)n;
 }
 if(halfClose)::shutdown(c,SD_SEND);
 std::string reply;char b[1024];for(int n;(n=::recv(c,b,sizeof(b),0))>0;)reply.append(b,(size_t)n);::closesocket(c);
 auto space=reply.find(' ');return space==std::string::npos?0:std::atoi(reply.c_str()+space+1);
}
int main(){try{
 WebUi ui; if(!ui.start(0,[](const auto&,auto&){return "{}";}))throw std::runtime_error("start");
 const std::string prefix="POST /probe HTTP/1.1\r\nHost: localhost\r\nX-Pad: ";
 for(size_t size:{kMaxHeadBytes-1,kMaxHeadBytes,kMaxHeadBytes+1,kMaxHeadBytes+2048}){
  const int status=request(ui.port(),prefix+std::string(size-prefix.size(),'x')+"\r\n\r\n");
  check(status==(size>kMaxHeadBytes?431:200),"header limit is enforced even when terminator arrives in last recv");
 }
 check(request(ui.port(),prefix+std::string(kMaxHeadBytes-prefix.size(),'x')+"\r\n\r\n",false,true)==200,"exact header limit accepts fragmented terminator");
 for(const char* path:{"/api/uploadXYZ","/api/upload/child","/api/play"}){
  check(request(ui.port(),std::string("POST ")+path+" HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16777217\r\n\r\n")==413,"only the exact upload route gets the large-body allowance");
 }
 for(const char* path:{"/api/upload","/api/upload?name=x"}){
  check(request(ui.port(),std::string("POST ")+path+" HTTP/1.1\r\nHost: localhost\r\nContent-Length: 16777217\r\n\r\n")==400,"real upload accepts large declaration then rejects incomplete body");
 }
 for(const char* length:{"","-1","+1","1x"})
  check(request(ui.port(),std::string("POST /probe HTTP/1.1\r\nContent-Length: ")+length+"\r\n\r\n")==400,"invalid content length is rejected");
 check(request(ui.port(),"POST /probe HTTP/1.1\r\nContent-Length: 4\r\n\r\nabc")==400,"short request body is rejected");
 check(request(ui.port(),"POST /probe HTTP/1.1\r\ncontent-length: 3\r\n\r\nabc")==200,"lowercase length and exact body succeed");
 check(request(ui.port(),"POST /probe HTTP/1.1\r\nContent-Length: 3\r\n\r\nabc",true)==200,"complete valid request supports a half-closed sender");
 ui.stop();std::cout<<(failures?"[FAIL] ":"[PASS] ")<<"HTTP boundary checks="<<checks<<" failures="<<failures<<'\n';return failures?1:0;
 }catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
