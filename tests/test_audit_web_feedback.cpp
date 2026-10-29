#ifdef NDEBUG
#undef NDEBUG // The regression must execute all operations in Release builds as well.
#endif
#include <cassert>
#include <iostream>
#include <fstream>
#include "net/feedback_redaction.h"
#ifdef _WIN32
#include "net/feedback.h"
#endif
using namespace rta::feedback;
static std::string decode64(const std::string& s) {
 const std::string table="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 std::string out; unsigned value=0;int bits=0;
 for(unsigned char c:s){if(c=='=')break;auto x=table.find(c);assert(x!=std::string::npos);value=(value<<6)|(unsigned)x;bits+=6;if(bits>=8){bits-=8;out+=(char)(value>>bits);}}return out;
}
int main(int argc,char**argv) {
 const auto numbers=rta::net::json_parse("{\"large\":1e300,\"precise\":0.12345678901234566}");
 const auto roundtrip=rta::net::json_parse(sanitize_json_fragment("{\"large\":1e300,\"precise\":0.12345678901234566}"));
 assert(roundtrip["large"].n()==numbers["large"].n() && roundtrip["precise"].n()==numbers["precise"].n());
 const std::string secret="SyntheticDummyCredential987";
 std::string raw="{";int count=0;
 for(const char* key:{"password","passwd","token","access_token","refresh_token","api_key","api-key","x-api-key","apikey","secret","client_secret","auth","key","gh_token","Authorization","Proxy-Authorization","Cookie"}) {
  if(count++)raw+=',';raw+='"'+std::string(key)+"\":\""+secret+'"';
 }
 raw+=" ,\"nested\":{\"password\":{\"plain\":\""+secret+"\"}},\"array\":[{\"api_key\":[\""+secret+"\",1]}],\"url\":\"https://u:"+secret+"@example.test/file\",\"ordinary\":\"keep me\",\"tokens_used\":123}";
 const std::string clean=sanitize_json_fragment(raw);
 assert(clean.find(secret)==std::string::npos);
 auto parsed=rta::net::json_parse(clean);assert(parsed["ordinary"].s()=="keep me" && parsed["tokens_used"].n()==123);
 assert(parsed["nested"]["password"].s()=="<redacted>");
 assert(parsed["array"].at(0)["api_key"].s()=="<redacted>");
 assert(redact("password="+secret).find(secret)==std::string::npos);
 assert(redact("https://user:"+secret+"@example.test/").find(secret)==std::string::npos);
 const auto wire=rta::net::json_parse(worker_request_body("token="+secret,"ordinary", "2.0.0", "test", clean));
 assert(wire["text"].s().find(secret)==std::string::npos);
 assert(decode64(wire["diag"].s())==clean);
#ifdef _WIN32
 assert(argc==2);const std::string root=argv[1];
 assert(root.find("review-fixes")!=std::string::npos || root.find("audit-fixtures")!=std::string::npos); // isolated test roots only
 std::filesystem::create_directories(root);
 const auto conf=root+"\\synthetic-settings.conf";
 {std::ofstream f(conf);f<<"password="<<secret<<"\nordinary=keep me\n";}
 bool present=false;const auto snap=settings_snapshot(conf,present);assert(present && snap.find(secret)==std::string::npos);
 UserReport report;report.title="synthetic audit";report.body="token="+secret;
 LiveState live;live.settingsJson=raw;live.extraJson=raw;live.endpointsJson="["+raw+"]";live.gpu="synthetic";
 Options opt;opt.dryRun=true;opt.skipTokenLookup=true;opt.logFiles={{"synthetic",root+"\\absent.log"}};
 // exe lives in the isolated review directory: default local settings/tester files are absent.
 const auto result=submit(report,live,opt);assert(!result.uploaded && result.status==0 && result.json.find(secret)==std::string::npos);
 const auto local=outbox_write(result.id,result.json,root+"\\outbox");assert(!local.empty());
 std::ifstream f(local,std::ios::binary);const std::string stored((std::istreambuf_iterator<char>(f)),{});assert(stored==result.json && stored.find(secret)==std::string::npos);
 const auto finalWire=rta::net::json_parse(worker_request_body(report.title,report.body,"2.0.0",live.source,result.json));
 assert(decode64(finalWire["diag"].s()).find(secret)==std::string::npos);
 std::cout<<"PASS Windows settings, production payload, local outbox and final Worker request (no HTTP sent)\n";
#else
 (void)argc;(void)argv;
#endif
 std::cout<<"PASS "<<count<<" sensitive field names, nested objects/arrays, URL credentials, ordinary fields, exact finite-number roundtrip and wire body\n";
}
