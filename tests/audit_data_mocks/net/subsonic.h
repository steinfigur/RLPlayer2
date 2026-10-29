#pragma once
#include <string>
#include <vector>
namespace rta::net {
inline int downloads=0;
struct HttpResponse { bool ok=true; std::vector<unsigned char> body; std::string error,contentType="audio/flac"; };
struct SubEntry {};
struct SubConfig { std::string base,user,pass; };
class Subsonic { SubConfig c; public: Subsonic()=default; Subsonic(SubConfig x):c(x){} void set_config(SubConfig x){c=x;} bool ping(std::string&)const{return true;} std::vector<SubEntry> browse(const std::string&,std::string&){return {};} std::vector<SubEntry> indexes(std::string&){return {};} std::vector<SubEntry> search(const std::string&,std::string&){return {};} HttpResponse cover(const std::string&,int){return {};} HttpResponse download(const std::string& id){++downloads; HttpResponse r;std::string s=c.base+":"+c.user+":"+c.pass+":"+id;r.body.assign(s.begin(),s.end());return r;} };
}
