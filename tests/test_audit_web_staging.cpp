#ifdef NDEBUG
#undef NDEBUG // The regression must execute all operations in Release builds as well.
#endif
#include <cassert>
#include <iostream>
#include "net/update_staging.h"
using namespace rta::update;
int main(){
 int checks=0;std::string err;StagingManifest out;
 StagingManifest m;m.version="2.0.0";m.installDir="synthetic-install";
 m.files={{{"a.dll",sha256_hex("a"),1},true},{{"b.dll",sha256_hex("b"),1},false}};
 const auto text=serialize_staging_manifest(m);assert(parse_staging_manifest(text,out,err));
 assert(validate_staging_manifest(out,m.version,m.installDir,[](const auto&){return true;},err));++checks;
 assert(!validate_staging_manifest(out,m.version,m.installDir,[](const auto&e){return e.file.path!="b.dll";},err));++checks;
 assert(!validate_staging_manifest(out,"2.0.1",m.installDir,[](const auto&){return true;},err));++checks;
 assert(!validate_staging_manifest(out,m.version,"other",[](const auto&){return true;},err));++checks;
 for(const char*p:{"../a","C:\\a","a:b","a\\..\\b","a\\","a//b","a. ","CON.txt","aux","a\nb"}){assert(!safe_install_rel(p));++checks;}
 m.files.push_back({{"A.DLL",sha256_hex("a"),1},true});assert(!parse_staging_manifest(serialize_staging_manifest(m),out,err));++checks;
 m.files.pop_back();m.files[0].staged=false;assert(!validate_staging_manifest(m,m.version,m.installDir,[](const auto&){return true;},err));++checks;
 std::cout<<"PASS "<<checks<<" portable staging contract cases\n";
}
