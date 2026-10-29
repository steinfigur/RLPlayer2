#pragma once
#include "net/subsonic.h"
namespace rta::net {struct EmbyConfig {std::string base,user,pass;}; class Emby:public Subsonic {public:Emby()=default;Emby(EmbyConfig c):Subsonic(SubConfig{c.base,c.user,c.pass}){} void set_config(EmbyConfig c){Subsonic::set_config(SubConfig{c.base,c.user,c.pass});}bool login(std::string&){return true;} };}
