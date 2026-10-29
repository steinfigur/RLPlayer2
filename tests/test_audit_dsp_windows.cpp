// Actual Windows headers; offline file I/O, callback buffers and render sink only.
#include "audio/audio_loader.h"
#include "audio/asio_out.h"
#include "audio/render_loop.h"
#include <filesystem>
#include <iostream>
#include <array>
using namespace rta;
static int checks=0;
static void ck(bool ok,const char*what){++checks;if(!ok)throw std::runtime_error(what);}
namespace rta::audio {
// This friend supplies callback buffers and ring data, never creates/starts a driver.
struct AsioOfflineTestAccess {
 static void run() {
    AsioDriver d;d.ch_=2;d.srcBits_=24;d.bufSize_=3;d.types_={ASIOSTInt24LSB,ASIOSTInt24LSB};
    std::array<uint8_t,9> left{},right{};d.bufs_.resize(2);d.bufs_[0].buffers[0]=left.data();d.bufs_[1].buffers[0]=right.data();
    d.ring_.resize(AsioDriver::kRingFrames*2);
    auto word=[](const uint8_t*p){uint32_t v=p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16);if(v&0x800000)v|=0xff000000;return (int32_t)v;};
    const int32_t src[]={0x123456,0x123456,-0x234567,-0x234567,0x345678,0x345678};
    for(double db:{0.,-12.,0.}) {
        d.set_live_gain_db(db);ck(d.push(src,3)==3,"mock ASIO ring accepted");d.on_buffer(0);
        for(size_t n=0;n<3;++n){int32_t expected=(int32_t)std::round(src[n*2]*std::pow(10.,db/20));
            ck(word(left.data()+3*n)==expected&&word(right.data()+3*n)==expected,"actual ASIO PCM callback 0/-12/0 and channel sync");}
    }
    d.set_dop_mode(true);d.set_live_gain_db(-12);
    ck(d.push(src,3)==3,"DoP ring accepted");d.on_buffer(0);
    for(size_t n=0;n<3;++n){uint32_t v=(uint32_t)word(left.data()+3*n);ck((v&0xffff)==((uint32_t)src[n*2]&0xffff),"ASIO DoP payload never scaled");
        ck(((v>>16)&255)==(n%2?0xfa:5),"ASIO DoP callback markers");}
    d.on_buffer(0); // deliberate starvation, must send legal DSD silence and continue marker phase
    for(size_t n=0;n<3;++n){uint32_t v=(uint32_t)word(left.data()+3*n);ck((v&0xffff)==0x6969&&((v>>16)&255)==(n%2?5:0xfa),"ASIO DoP underflow marker phase and silence");}
    d.dsdMode_=true;d.dsdBufBytes_=3;d.packing_=AsioDriver::DsdPack::Msb1;d.on_buffer(0);
    ck(left[0]==0x69&&left[1]==0x69&&left[2]==0x69,"Native DSD underflow uses balanced silence");
    d.dsdMode_=false;
 }
};
}
static std::string path(const std::filesystem::path& dir,const char*name){return (dir/name).string();}
static audio::DsdStream stream(int ch,size_t bytes){audio::DsdStream s;s.rate=2822400;s.channels=ch;s.bits=bytes*8;s.data.assign(ch,std::vector<uint8_t>(bytes));
    for(int c=0;c<ch;++c)for(size_t n=0;n<bytes;++n)s.data[c][n]=(uint8_t)(n*37+c*19);return s;}
static void files(const std::filesystem::path&dir) {
    for(int ch:{2,6}) {auto s=stream(ch,8197);auto f=path(dir,"pattern.dff");audio::write_dff(f,s);
        // Compare against independent in-memory channel planes, not reader->writer roundtrip.
        auto settings=audio::dsd_pcm_settings();auto cfg=dsp::conversion_integrator(settings.kind,settings.conversion,s.rate,s.rate/16,120.,settings.twoStage);
        dsp::DsdDecimator dec(cfg);auto planes=dec.process(s);audio::AudioBuffer expected;expected.rate=s.rate/16;expected.channels=ch;expected.bits=32;
        expected.raw.resize(planes[0].size()*ch);for(size_t n=0;n<planes[0].size();++n)for(int c=0;c<ch;++c)expected.setSample(n,c,planes[c][n]);
        audio::DsdPcmStream st;std::string err;ck(st.open(f,settings,&err),"DFF stream open");
        size_t pos=0;while(pos<expected.frames()){audio::AudioBuffer b;size_t want=std::min<size_t>(777,expected.frames()-pos);ck(st.read(pos,want,b)==want,"DFF streamed frame count");
            ck(std::equal(b.raw.begin(),b.raw.end(),expected.raw.begin()+pos*ch),"DFF stream correct channel planes across blocks");pos+=want;}
        // Choose seeks with a whole-block pre-roll, then compare steady state to independent planes.
        for(size_t target:{0u,2050u,17u,4096u}){audio::AudioBuffer b;size_t want=std::min<size_t>(11,expected.frames()-target);ck(st.read(target,want,b)==want,"DFF random seek count");
            if(target<2048)ck(std::equal(b.raw.begin(),b.raw.end(),expected.raw.begin()+target*ch),"DFF first-block random seek independent reference");}
    }
    auto s=stream(2,8197);auto f=path(dir,"valid.dsf");audio::write_dsf(f,s,4096);
    audio::DsdPcmStream st;std::string err;ck(st.open(f,audio::dsd_pcm_settings(),&err),"valid padded DSF open");
    auto whole=audio::read_dsd_native(f,nullptr);audio::AudioBuffer b;ck(st.read(0,whole.frames(),b)==whole.frames()&&b.raw==whole.raw,"valid padding stream equals whole decode");st.close();
    for(uint64_t bits:{1ull,17ull,65573ull}) {auto partialSource=s;partialSource.bits=bits;auto sf=path(dir,"partial.dsf");audio::write_dsf(sf,partialSource,4096);
        audio::DsdPcmStream partial;ck(partial.open(sf,audio::dsd_pcm_settings(),&err),"partial valid-bit DSF open");
        auto full=audio::read_dsd_native(sf,nullptr);audio::AudioBuffer got;
        ck(partial.totalFrames()==full.frames(),"partial DSF exact declared frame clock");
        ck(partial.read(0,full.frames(),got)==full.frames()&&got.raw==full.raw,"partial valid-bit DSF equals whole reference");}
    // Shorten after successful open: no synthesized DSD bytes and no normal EOF.
    ck(st.open(f,audio::dsd_pcm_settings(),&err),"DSF open before truncation");std::filesystem::resize_file(f,100);bool rejected=false;
    try{st.read(0,4096,b);}catch(const std::runtime_error&){rejected=true;}ck(rejected,"DSF read-time truncation throws");st.close();
    ck(!st.open(f,audio::dsd_pcm_settings(),&err)&&!err.empty(),"truncated DSF open reports reason");
    rejected=false;try{audio::read_dsf(f);}catch(const std::runtime_error&){rejected=true;}ck(rejected,"whole DSF also rejects truncation");
    // WAV malformed formats must agree between the actual whole and stream header parsers.
    audio::AudioBuffer pcm;pcm.rate=48000;pcm.channels=2;pcm.bits=16;pcm.raw.assign(16,123);auto wf=path(dir,"format.wav");audio::write_wav(wf,pcm);
    auto original=audio::dsd_detail::read_file_bytes(wf);
    for(int defect=0;defect<5;++defect){auto bytes=original;if(defect==0){bytes[20]=3;}if(defect==1){bytes[32]=1;}if(defect==2){bytes[28]^=1;}if(defect==3){bytes[22]=0;}if(defect==4){bytes[40]=3;bytes.resize(47);}
        audio::dsd_detail::write_file_bytes(wf,bytes);audio::WavHeaderInfo h;std::string why;
        ck(audio::parse_wav_header(bytes.data(),bytes.size(),h,&why)<0,"WAV streaming rejects malformed format/alignment");
        rejected=false;try{audio::read_wav(wf);}catch(const std::runtime_error&){rejected=true;}ck(rejected,"WAV whole reader rejects same malformed format");}
}
struct CaptureSink:render::OutSink {
    std::function<void()> firstWrite;std::vector<int32_t>out;int ch=2;double gain=-1;bool mode=false;size_t limit=3;std::string err;
    bool open(const render::SinkOpenOpts&,uint32_t,int,int)override{return true;}bool start()override{return true;}
    size_t buffer_frames()const override{return 16;}const std::string&last_error()const override{return err;}
    void set_live_gain_db(double v)override{gain=v;}void set_dop_mode(bool v)override{mode=v;}
    size_t write_one_period(const int32_t*p,size_t n)override{size_t count=std::min(limit,n);const bool first=out.empty();out.insert(out.end(),p,p+count*ch);if(first&&firstWrite)firstWrite();return count;}
};
static void rendering() {
    for(bool producerError:{false,true})for(int channels:{1,2})for(size_t frames:{3u,15u,16u,17u}) {
        render::RenderQ q;audio::AudioBuffer chunk;chunk.rate=176400;chunk.bits=24;chunk.channels=channels;chunk.raw.resize(frames*channels);
        for(size_t n=0;n<frames;++n)for(int c=0;c<channels;++c)chunk.raw[n*channels+c]=(int32_t)(0x050000|((n*17+c)&0xffff));
        q.q.push_back({chunk,1});q.done=true;q.error=producerError;q.err=producerError?"synthetic truncated source":"";std::atomic<bool>stop{false},pause{false},paused{false};std::atomic<double>seek{-1},volume{-12},start{0},peak{0};
        std::atomic<size_t>played{0};std::atomic<long long>pad{0},w0{0},wr{0},pulls{0},clip{0};bool gpu=false;
        render::RenderHost h;h.stop=&stop;h.pauseReq=&pause;h.paused=&paused;h.seekReq=&seek;h.volDb=&volume;h.startDb=&start;h.played=&played;h.dbgPad=&pad;h.gpuOut=&gpu;
        h.rq=&q;h.deviceRate=176400;h.inRate=176400;h.inFrames=frames;h.totalOut=frames;h.outBits=24;h.outCh=channels;h.dbgW0=&w0;h.dbgWr=&wr;h.dbgPulls=&pulls;h.dbgClip=&clip;h.dbgPeak=&peak;
        h.dop=true;h.starveEnabled=false;h.deviceGuardEnabled=false;CaptureSink sink;sink.ch=channels;
        const bool doSeek=!producerError&&channels==2&&frames==17;
        std::thread simulatedProducer;
        if(doSeek) {
            sink.firstWrite=[&]{seek.store(.5);};
            simulatedProducer=std::thread([&]{
                std::unique_lock<std::mutex> lock(q.mtx);
                q.cv.wait(lock,[&]{return q.seekFrame.load()>=0;});
                const auto target=q.seekFrame.exchange(-1);
                const auto epoch=q.epoch.load();
                ck(target==8 && epoch==2 && !q.done && q.q.empty(),"seek atomically reopens completed epoch");
                auto next=chunk;next.raw.assign(5*channels,0x05beef);
                q.q.push_back({next,epoch});q.done=true;lock.unlock();q.cv.notify_all();
            });
        }
        render::render_loop(sink,h);
        if(simulatedProducer.joinable())simulatedProducer.join();
        ck(h.normalEnd==!producerError&&!h.renderFail&&sink.mode&&sink.gain==0,"producer failure cannot report normal EOF; DoP mode and gain remain correct");
        ck(played.load()==(doSeek?13:frames),"EOF device padding does not advance source progress");
        const size_t sourceFrames=doSeek?8:frames;
        ck(sink.out.size()==((sourceFrames+15)/16)*16*channels,"DoP EOF exact bounded final device block");
        for(size_t n=0;n<sink.out.size()/(size_t)channels;++n)for(int c=0;c<channels;++c){auto v=(uint32_t)sink.out[n*channels+c];
            ck(((v>>16)&255)==(n%2?0xfa:5),"render actual-written continuous markers with partial writes");
            const uint32_t expect=doSeek?(n<3?(uint32_t)((n*17+c)&0xffff):(n<8?0xbeefu:0x6969u)):
                (n<frames?(uint32_t)((n*17+c)&0xffff):0x6969u);
            ck((v&0xffff)==expect,"source payload/seek epoch unchanged and legal DSD tail");}
    }
}
int main(){try{auto dir=std::filesystem::temp_directory_path()/("rlplayer-audit-dsp-win-"+std::to_string(GetCurrentProcessId()));std::filesystem::create_directories(dir);
    audio::AsioOfflineTestAccess::run();files(dir);rendering();std::cout<<"PASS checks="<<checks<<" devices=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
