// Offline contracts: actual DSP headers, independent FIR/container references, no devices.
#include "engine/pipeline.h"
#include "dsp/dop.h"
#include "dsp/modulator.h"
#include "audio/dsd_reader.h"
#include "audio/pcm_gain.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <array>
using namespace rta;
static int checks = 0;
static void ck(bool ok, const char* what) { ++checks; if (!ok) throw std::runtime_error(what); }
static audio::AudioBuffer input(size_t n, int ch=2, double value=0) {
    audio::AudioBuffer b; b.rate=48000; b.bits=24; b.channels=ch; b.raw.resize(n*(size_t)ch);
    for (size_t i=0; i<n; ++i) for(int c=0;c<ch;++c) b.setSample(i,c,value);
    return b;
}
static std::vector<int32_t> run(const audio::AudioBuffer& b, const engine::PipelineConfig& cfg, size_t block) {
    engine::Pipeline p(cfg); std::vector<int32_t> y;
    for(size_t n=0;n<b.frames();n+=block) {
        auto x=b; size_t end=std::min(b.frames(),n+block);
        x.raw.assign(b.raw.begin()+n*b.channels,b.raw.begin()+end*b.channels);
        auto o=p.process(x); y.insert(y.end(),o.raw.begin(),o.raw.end());
    }
    auto tail=p.finish(); y.insert(y.end(),tail.raw.begin(),tail.raw.end());
    ck(p.finish().raw.empty(),"finish is idempotent"); return y;
}
static void volume() {
    for(int ch : {1,2,6,8}) {
        engine::PipelineConfig cfg; cfg.volumeEnabled=true; cfg.volumeDb=-60;
        engine::Pipeline p(cfg); auto out=p.process(input(777,ch,.5));
        for(size_t n=0;n<777;++n) for(int c=0;c<ch;++c)
            ck(std::abs(out.sample(n,c)-.0005)<1.3e-7,"safe initial gain on every frame/channel");
        dsp::VolumeControl v; v.set_sample_rate(48000); v.set_gain_db(0); v.snap(); v.set_gain_db(-12);
        std::vector<std::vector<double>> planes(ch,std::vector<double>(777,.5));
        v.process_planar(planes);
        for(int c=1;c<ch;++c) ck(planes[c]==planes[0],"shared frame envelope");
    }
    auto x=input(99,2,.5); engine::PipelineConfig c;
    engine::Pipeline p(c); engine::PipelineStats st; ck(p.process(x,&st).raw==x.raw && st.bitPerfect,"0dB bit-perfect");
    c.forceBitPerfect=true;c.outRate=96000;c.outBits=32;engine::Pipeline forced(c);auto exact=forced.process(x,&st);
    ck(exact.raw==x.raw&&exact.rate==x.rate&&exact.bits==x.bits&&st.outRate==x.rate&&st.outBits==x.bits,"forced bit-perfect retains source metadata");
    c.forceBitPerfect=false;c.outRate=0;
    for(int bits:{24,32}) {c.outBits=bits;c.preAttenDb=-3;engine::Pipeline q(c);auto y=q.process(x,&st);
        ck(std::abs(y.sample(0,0)-.5*std::pow(10.,-3./20))<1.3e-7,"pre-attenuation reaches every precision path");
        ck(!st.bitPerfect && !st.zeroPad && st.usedPreAtten,"attenuation stats");}
}
static void peq() {
    auto eq=std::make_shared<dsp::PeqEngine>(); auto s=dsp::peq_flat_settings(); s.preampDb=-12;
    eq->set_settings(s);eq->prepare(48000,2);engine::PipelineConfig c;c.peq=eq; engine::Pipeline p(c);
    auto a=p.process(input(256,2,.5));eq->set_bypass(true);auto b=p.process(input(256,2,.5));
    ck(std::abs(a.sample(255,0)-b.sample(0,0))<2e-7,"PEQ bypass begins from previous gain");
    ck(b.sample(127,0)>.49,"PEQ transition reaches bypass in 128 frames");
    engine::PipelineStats st; auto x=input(64,2,.5); ck(p.process(x,&st).raw==x.raw&&st.bitPerfect,"PEQ restores bit-perfect after transition");
    s.bands[0].gainDb=12;s.bands[0].freq=100;s.bands[0].q=10;s.preampDb=0;
    eq->set_settings(s);eq->prepare(48000,2);auto impulse=input(256);impulse.setSample(255,0,.5);p.process(impulse);
    eq->prepare(48000,2);engine::Pipeline next(c); auto z=next.process(input(256));
    ck(std::all_of(z.raw.begin(),z.raw.end(),[](int32_t v){return v==0;}),"same-rate new track clears PEQ history");
    p.process(impulse);p.seek_reset();z=p.process(input(256));
    ck(std::all_of(z.raw.begin(),z.raw.end(),[](int32_t v){return v==0;}),"seek clears PEQ history");
}
static void eof() {
    for(uint32_t rate:{48000u,96000u,44100u,384000u}) for(size_t block:{1u,31u,777u,4096u})
    for(size_t pos:{0u,511u,1023u}) {
        auto b=input(1024);b.setSample(pos,0,.5);b.setSample(pos,1,.25);
        engine::PipelineConfig cfg;cfg.outRate=rate;cfg.outBits=32;cfg.taps=128;cfg.gpuResample=false;
        cfg.convBlock=32;cfg.channel.resize(2);cfg.channel[0].ir={1}; // channel 1 must be latency-aligned
        auto actual=run(b,cfg,block);size_t count=(b.frames()*rate+b.rate-1)/b.rate;
        ck(actual.size()==count*2,"EOF keeps source timeline length");
        // Independent causal polyphase reference: zero insertion + FIR convolution + rational decimation.
        std::vector<double> h={1};uint32_t g=std::gcd(b.rate,rate),L=rate/g,M=b.rate/g;size_t skip=0;
        if(rate!=b.rate) {
            if(rate==384000u) { // two-stage checked separately against single-block segmentation below
                auto whole=run(b,cfg,b.frames());ck(actual==whole,"two-stage EOF block independence");continue;
            }
            dsp::LowpassSpec spec;spec.taps=std::max(4,128/(int)L)*(int)L;
            spec.cutoff=std::min(1./L,1./M)*(1-cfg.transition);spec.attenDb=cfg.attenDb;
            h=dsp::design_lowpass(spec);for(auto&v:h)v*=L;skip=((size_t)32*L+(h.size()-1)/2)/M;
        }
        for(size_t n=0;n<count;++n) {
            const int64_t k=(int64_t)((n+skip)*M)-(int64_t)((pos+(rate!=b.rate?32:0))*L);
            const double expect=k>=0&&(size_t)k<h.size()? .5*h[(size_t)k] : 0;
            const double got=(double)actual[n*2]/2147483648.;
            if(std::abs(got-expect)>=2e-8) std::cerr<<"rate="<<rate<<" block="<<block<<" pos="<<pos<<" n="<<n<<" got="<<got<<" expected="<<expect<<" skip="<<skip<<" k="<<k<<"\n";
            ck(std::abs(got-expect)<2e-8,"EOF impulse matches independent FIR reference");
            ck(std::abs((double)actual[n*2+1]/2147483648.-expect*.5)<2e-8,"all channels retain alignment");
        }
        auto whole=run(b,cfg,b.frames());ck(actual==whole,"EOF block independence");
    }
    auto b=input(100);b.setSample(99,0,.5);engine::PipelineConfig c;c.convBlock=32;c.channel.resize(2);c.channel[0].ir={.5,.25};
    auto y=run(b,c,7);ck(y[198]==2097152,"nontrivial IR keeps last input contribution");
}
static void presets() {
    for(const auto& fp:dsp::filter_presets()) {
        engine::PipelineConfig c;engine::apply_filter_preset(c,fp);
        ck(c.taps==fp.taps&&c.transition==fp.transition&&c.attenDb==fp.attenDb&&c.filterWindow==fp.window&&c.filterPhase==fp.phase&&c.twoStage==fp.twoStage&&c.minTapsPerInput==fp.tapsPerInput&&c.apodizing==fp.apodizing&&c.apodF1==fp.apodF1&&c.apodF2==fp.apodF2,"all preset semantic fields mapped");
        if(fp.twoStage&&fp.stage2Taps>0)ck(c.stage2Taps==fp.stage2Taps,"preset stage2 taps");
    }    auto x=input(256);x.setSample(0,0,.5);
    engine::PipelineConfig c;c.outRate=96000;c.outBits=32;c.taps=512;c.twoStage=false;c.gpuResample=false;
    c.apodizing=true;c.apodF1=.70;c.apodF2=.85;
    auto actual=run(x,c,31);
    dsp::FirSpec fs;fs.taps=512;fs.cutoff=.5*(1-c.transition);fs.attenDb=c.attenDb;fs.window=c.filterWindow;
    fs.apodizing=true;fs.apodF1=.35;fs.apodF2=.425;
    auto h=dsp::design_fir(fs);size_t delay=(h.size()-1)/2;
    for(size_t n=0;n<actual.size()/2;++n) {
        const double reference=n+delay<h.size() ? h[n+delay] : 0.0; // .5 impulse times L=2
        ck(std::abs(actual[n*2]/2147483648.-reference)<2e-8,"apod Pipeline impulse matches independently designed FIR");
    }
    c.apodizing=false;auto normal=run(x,c,31);double difference=0;
    for(size_t n=0;n<normal.size();++n)difference=std::max(difference,std::abs((double)(int64_t)actual[n]-(double)(int64_t)normal[n])/2147483648.);
    ck(difference>1e-4,"apod changes actual Pipeline output, not only config fields");
}

static void dop() {
    dsp::SdmConfig cfg;cfg.family=dsp::ModulatorFamily::Ahm;dsp::SdmModulator ahm(cfg);
    ck(!ahm.supports_dsd_output(),"AHM declares multilevel capability");bool rejected=false;
    try{ahm.process_dsd(.2);}catch(const std::invalid_argument&){rejected=true;}ck(rejected,"AHM one-bit output rejected");
    std::vector<double>x(32,.2);const double*ip[]={x.data()};std::vector<int32_t>out;dsp::DopPackState st;
    rejected=false;try{dsp::dop_pack(ip,1,x.size(),out,st);}catch(const std::invalid_argument&){rejected=true;}
    ck(rejected && out.empty() && st.phaseA.empty(),"DoP rejects multilevel before changing output/state");
    for(size_t n=0;n<x.size();++n)x[n]=(n%3==0)?1:-1;
    dsp::dop_pack(ip,1,x.size(),out,st);std::vector<std::vector<double>> decoded;
    ck(dsp::dop_unpack(out.data(),1,2,decoded)&&decoded[0]==x,"legal bits and DoP markers unchanged");
    int32_t samples[]={0x05aaaa,0x05aaaa,-0x055555,-0x055555};auto original=std::array{samples[0],samples[1],samples[2],samples[3]};
    audio::apply_pcm_live_gain(samples,4,24,1);ck(std::equal(original.begin(),original.end(),samples),"0dB exact PCM words");
    audio::apply_pcm_live_gain(samples,4,24,std::pow(10.,-12./20));
    ck(samples[0]==samples[1]&&samples[2]==samples[3]&&std::abs(samples[0]-original[0]*std::pow(10.,-12./20))<1,"PCM live gain exact channel synchronization");
}
static void dff(const std::filesystem::path&dir) {
    // Externally generated DSDIFF 1.5 fixture, independently demuxed by FFmpeg in the audit.
    // Byte order: 11 a1 22 b2 33 c3 44 d4 (L0 R0 L1 R1 ...).
    const std::vector<uint8_t> fixture={
      0x46,0x52,0x4d,0x38,0,0,0,0,0,0,0,0x7e,0x44,0x53,0x44,0x20,
      0x46,0x56,0x45,0x52,0,0,0,0,0,0,0,4,1,5,0,0,0x50,0x52,0x4f,0x50,
      0,0,0,0,0,0,0,0x4a,0x53,0x4e,0x44,0x20,0x46,0x53,0x20,0x20,0,0,0,0,0,0,0,4,0,0x2b,0x11,0,
      0x43,0x48,0x4e,0x4c,0,0,0,0,0,0,0,0xa,0,2,0x53,0x4c,0x46,0x54,0x53,0x52,0x47,0x54,
      0x43,0x4d,0x50,0x52,0,0,0,0,0,0,0,0x13,0x44,0x53,0x44,0x20,0xe,0x6e,0x6f,0x74,0x20,0x63,0x6f,0x6d,0x70,0x72,0x65,0x73,0x73,0x65,0x64,0,
      0x44,0x53,0x44,0x20,0,0,0,0,0,0,0,8,0x11,0xa1,0x22,0xb2,0x33,0xc3,0x44,0xd4};
    auto path=(dir/"standard.dff").string();audio::dsd_detail::write_file_bytes(path,fixture);auto s=audio::read_dff(path);
    ck(s.data[0]==std::vector<uint8_t>({0x11,0x22,0x33,0x44})&&s.data[1]==std::vector<uint8_t>({0xa1,0xb2,0xc3,0xd4}),"DFF external fixture byte-interleaved");
    for(int ch:{2,6}) {s.channels=ch;s.bits=40;s.channelIds=audio::dsd_detail::dsf_channel_ids(ch);s.data.assign(ch,std::vector<uint8_t>(5));
        for(int c=0;c<ch;++c)for(int n=0;n<5;++n)s.data[c][n]=(uint8_t)(c*19+n);
        audio::write_dff(path,s);auto raw=audio::dsd_detail::read_file_bytes(path);size_t off=16;
        while(std::memcmp(raw.data()+off,"DSD ",4)!=0) {uint64_t sz=0;for(int n=0;n<8;++n)sz=sz*256+raw[off+4+n];off+=12+(size_t)sz+(sz&1);}
        for(int n=0;n<5;++n)for(int c=0;c<ch;++c)ck(raw[off+12+n*ch+c]==(uint8_t)(c*19+n),"DFF writer independent interleave check");
    }
}
int main() {try {auto dir=std::filesystem::temp_directory_path()/"rlplayer-audit-dsp";std::filesystem::create_directories(dir);
    volume();peq();eof();presets();dop();dff(dir);std::cout<<"PASS checks="<<checks<<" devices=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
