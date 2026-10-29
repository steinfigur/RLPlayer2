#ifdef NDEBUG
#undef NDEBUG // The regression must execute all operations in Release builds as well.
#endif
// Compile the complete production WebUi with deterministic WinSock barriers.
#include <cassert>
#include <iostream>
#include <stdexcept>
#include "net/webui.h"
using namespace rta::net;
using namespace audit_net;
static std::string request(const char* path) { return std::string("GET ")+path+" HTTP/1.1\r\nHost: localhost\r\n\r\n"; }
static void closed(SOCKET s) { wait([&]{return clients.at(s)->closed;}); }
static std::string output(SOCKET s) { std::lock_guard<std::mutex> l(mx); return clients.at(s)->output; }
int main() {
    int checks=0;
    { // Stop must cancel a blocked receive and join before releasing the object.
        auto ui=std::make_unique<WebUi>(); assert(ui->start(0,[](const auto&,auto&){return "{}";}));
        auto s=add("GET / HTTP/1.1\r\n",true);
        wait([&]{return clients.at(s)->recvEntered;});
        ui->stop(); ui.reset(); closed(s); ++checks;
    }
    { // Stop must wait for an in-flight handler without holding its socket lock.
        WebUi ui; std::mutex gate; std::condition_variable changed; bool entered=false,release=false;
        assert(ui.start(0,[&](const auto&,auto&){std::unique_lock<std::mutex> l(gate);entered=true;changed.notify_all();changed.wait(l,[&]{return release;});return "{}";}));
        auto s=add(request("/handler"));
        {std::unique_lock<std::mutex> l(gate);assert(changed.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}
        std::atomic<bool> stopped{false}; std::thread stopper([&]{ui.stop();stopped=true;});
        wait([&]{return clients.at(s)->shut;});assert(!stopped.load());
        {std::lock_guard<std::mutex> l(gate);release=true;changed.notify_all();}
        stopper.join();closed(s);assert(stopped);++checks;
    }
    { // A blocked send is cancelled as well.
        WebUi ui;assert(ui.start(0,[](const auto&,auto&){return std::string(200000,'a');}));
        auto s=add(request("/send"),false,true);wait([&]{return clients.at(s)->sendEntered;});ui.stop();closed(s);++checks;
    }
    { // A handler calling stop concurrently with its owner must never self-join.
        WebUi ui;std::mutex gate;std::condition_variable changed;bool entered=false,release=false;
        assert(ui.start(0,[&](const auto&,auto&){
            {std::unique_lock<std::mutex> l(gate);entered=true;changed.notify_all();changed.wait(l,[&]{return release;});}
            ui.stop();return "{}";
        }));
        auto s=add(request("/self-stop"));
        {std::unique_lock<std::mutex> l(gate);assert(changed.wait_for(l,std::chrono::seconds(5),[&]{return entered;}));}
        std::thread owner([&]{ui.stop();});wait([&]{return clients.at(s)->shut;});
        {std::lock_guard<std::mutex> l(gate);release=true;changed.notify_all();}
        owner.join();ui.stop();closed(s);++checks;
    }
    { // Rejection and success overlap deliberately; each owns its status and body.
        WebUi ui;std::mutex gate;std::condition_variable changed;bool rejected=false,success=false;
        assert(ui.start(0,[&](const auto& r,auto&)->std::string{
            if(r.path=="/reject") {webui_set_status(405);std::unique_lock<std::mutex> l(gate);rejected=true;changed.notify_all();changed.wait(l,[&]{return success;});return "rejected";}
            if(r.path=="/throw") {webui_set_status(405);throw std::runtime_error("test");}
            webui_set_status(200);{std::lock_guard<std::mutex> l(gate);success=true;changed.notify_all();}return "accepted";
        }));
        auto a=add(request("/reject"));
        {std::unique_lock<std::mutex> l(gate);assert(changed.wait_for(l,std::chrono::seconds(5),[&]{return rejected;}));}
        auto b=add(request("/ok"));closed(a);closed(b);
        assert(output(a).find("HTTP/1.1 405 ")==0 && output(a).find("rejected")!=std::string::npos);
        assert(output(b).find("HTTP/1.1 200 ")==0 && output(b).find("accepted")!=std::string::npos);++checks;
        auto c=add(request("/throw"));closed(c);assert(output(c).find("HTTP/1.1 500 ")==0);++checks;
        auto d=add("garbage\r\n\r\n");closed(d);assert(output(d).find("HTTP/1.1 400 ")==0);++checks;
        auto e=add(request("/ok"));closed(e);assert(output(e).find("HTTP/1.1 200 ")==0);ui.stop();++checks;
    }
    { // Backpressure keeps exactly six handlers active; the seventh connection is closed by stop.
        WebUi ui;std::mutex gate;std::condition_variable changed;int entered=0;bool release=false;
        assert(ui.start(0,[&](const auto&,auto&){std::unique_lock<std::mutex> l(gate);++entered;changed.notify_all();changed.wait(l,[&]{return release;});return "{}";}));
        std::vector<SOCKET> ids;for(int i=0;i<7;++i) ids.push_back(add(request("/capacity")));
        {std::unique_lock<std::mutex> l(gate);assert(changed.wait_for(l,std::chrono::seconds(5),[&]{return entered==6;}));}
        wait([&]{return incoming.empty();});
        std::thread owner([&]{ui.stop();});wait([&]{return clients.at(ids[0])->shut;});
        {std::lock_guard<std::mutex> l(gate);assert(entered==6);release=true;changed.notify_all();}
        owner.join();for(auto id:ids)closed(id);++checks;
    }
    {WebUi ui;
     failSocket=true;assert(!ui.start(0,{}));failSocket=false;
     failBind=true;assert(!ui.start(0,{}));failBind=false;
     failListen=true;assert(!ui.start(0,{}));failListen=false;
     assert(ui.start(0,{}));ui.stop();ui.stop();assert(ui.start(0,{}));ui.stop();++checks;}
    {WebUi ui;assert(ui.start(0,[](const auto&,auto&){return "{}";},false));
     auto s=add("GET / HTTP/1.1\r\nhOsT: localhost\r\n\r\n");closed(s);
     assert(output(s).find("HTTP/1.1 200 ")==0);ui.stop();++checks;}
    {HttpResponseStatus outer,inner;WebUiResponseScope a(outer);webui_set_status(409);
     {WebUiResponseScope b(inner);webui_set_status(403);}webui_set_status(405);
     assert(outer.code==405 && inner.code==403);++checks;}
    assert(startups==cleanups);
    for(const auto& c:clients) assert(c.second->closed);
    std::cout<<"PASS "<<checks<<" lifecycle/status cases; startup="<<startups<<" cleanup="<<cleanups<<"\n";
}
