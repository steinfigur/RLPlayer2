// Actual PipelineStats -> integrity contract. Offline, no devices or configuration state.
#include "engine/playback_integrity.h"
#include <iostream>
#include <stdexcept>
using namespace rta::engine;
static int checks=0;
static void ck(bool ok,const char*reason){++checks;if(!ok)throw std::runtime_error(reason);}
static bool contains(const PlaybackIntegrity& p,const char*text){return p.reason.find(text)!=std::string::npos;}
static rta::audio::AudioBuffer input(int bits=24){rta::audio::AudioBuffer b;b.rate=48000;b.bits=bits;b.channels=2;b.raw.assign(1024*2,0);for(size_t n=0;n<b.frames();++n)for(int c=0;c<2;++c)b.setSample(n,c,.25);return b;}
int main(){try{
    auto b=input();IntegritySource source{24,24,2,false};IntegrityOutput output;PipelineStats base;
    PipelineConfig plain;Pipeline p(plain);p.process(b,&base);
    ck(playback_integrity(base,source).state=="yes","actual bypass is intact");
    auto pending=base;pending.outFrames=0;ck(playback_integrity(pending,source).state=="unknown","zero-output preparation is pending");
    for(auto flag:{&PipelineStats::usedMatrix,&PipelineStats::usedEq,&PipelineStats::usedPeq,&PipelineStats::usedDelay,
                  &PipelineStats::usedConvolution,&PipelineStats::usedGpuConvolution,&PipelineStats::usedResample,&PipelineStats::usedVolume,&PipelineStats::usedChannelGain,
                  &PipelineStats::usedPreAtten,&PipelineStats::usedReplayGain,&PipelineStats::usedLoudness,&PipelineStats::usedShaper}){
        auto st=base;st.*flag=true;ck(playback_integrity(st,source).state=="no","every actual used stage blocks an intact claim");}
    struct Case{int kind;const char*reason;};
    for(const auto test:{Case{0,"PEQ"},Case{1,"每声道增益"},Case{2,"Matrix"},Case{3,"声道延迟"},Case{4,"预衰减"},Case{5,"重采样"},Case{6,"抖动/整形"}}){
        PipelineConfig cfg;cfg.gpuResample=false;cfg.taps=64;
        if(test.kind==0){cfg.peq=std::make_shared<rta::dsp::PeqEngine>();auto eq=rta::dsp::peq_flat_settings();eq.preampDb=-12;cfg.peq->set_settings(eq);cfg.peq->prepare(48000,2);}
        if(test.kind==1){cfg.channel.resize(2);cfg.channel[0].gainDb=-3;}
        if(test.kind==2)cfg.matrix={{0,0,1},{1,1,1}};
        if(test.kind==3){cfg.channel.resize(2);cfg.channel[0].delaySamples=7;}
        if(test.kind==4)cfg.preAttenDb=-3;
        if(test.kind==5)cfg.outRate=96000;
        if(test.kind==6)cfg.shaper=rta::dsp::Shaper::TPDF;
        Pipeline actual(cfg);PipelineStats st;actual.process(b,&st);auto result=playback_integrity(st,source);
        ck(result.state=="no"&&contains(result,test.reason),"actual Pipeline execution supplies its integrity reason");
    }
    // The 32-bit decoder container contains exact 16-bit source values. 24-bit output retains them.
    auto widened=input(32);PipelineConfig cfg;cfg.outBits=24;Pipeline q(cfg);PipelineStats st;auto retained=q.process(widened,&st);
    IntegritySource sixteen{16,32,2,false};auto result=playback_integrity(st,sixteen);
    ck(result.state=="yes"&&contains(result,"无损展宽"),"decoder container narrowing is not source precision loss");
    ck(retained.sample(0,0)==widened.sample(0,0),"independent sample check for exact source widening");
    sixteen.actualBits=32;ck(playback_integrity(st,sixteen).state=="no"&&contains(playback_integrity(st,sixteen),"真实位深降低"),"actual precision reduction is reported");
    auto unknown=source;unknown.actualBits=0;ck(playback_integrity(base,unknown).state=="unknown","unknown source precision is not intact");
    unknown=source;unknown.containerBits=16;ck(playback_integrity(base,unknown).state=="unknown","contradictory container precision is not intact");
    auto floatBuffer=input(32);floatBuffer.isFloat=true;
    for(size_t n=0;n<floatBuffer.frames();++n)for(int channel=0;channel<2;++channel)floatBuffer.setSample(n,channel,.25);
    IntegritySource floating{32,32,2,true};Pipeline normalFloat(plain);PipelineStats floatStats;normalFloat.process(floatBuffer,&floatStats);
    ck(playback_integrity(floatStats,floating).state=="no","actual float-to-integer conversion is not bit-perfect");
    auto exactConfig=plain;exactConfig.forceBitPerfect=true;Pipeline exactFloat(exactConfig);auto exact=exactFloat.process(floatBuffer,&floatStats);
    ck(exact.raw==floatBuffer.raw&&playback_integrity(floatStats,floating).state=="yes","actual forced float raw bypass remains bit-perfect");
    auto mono=source;mono.channels=1;ck(playback_integrity(base,mono).state=="no","source/output channel change is reported");
    IntegritySource decodedDsd{1,24,2,false};
    ck(playback_integrity(base,decodedDsd).state=="no"&&contains(playback_integrity(base,decodedDsd),"DSD"),"upstream DSD decoding cannot be called PCM bit-perfect");
    output.liveGainDb=-12;ck(playback_integrity(base,source,output).state=="no","actual live gain changes the conclusion");
    output.liveGainDb=0;ck(playback_integrity(base,source,output).state=="yes","zero live gain retains source words");
    for(const auto mode:{"dop","native"}){output.mode=mode;ck(playback_integrity(base,source,output).state=="no","PCM remodulation cannot claim DSD direct");
        output.directDsd=true;IntegritySource dsd{1,1,2,false};ck(playback_integrity(base,dsd,output).state=="direct","actual DSD direct preserves one-bit payload");
        output.liveGainDb=-3;ck(playback_integrity(base,dsd,output).state=="no","live scaling cannot coexist with DSD direct");output.liveGainDb=0;output.directDsd=false;}
    ck(playback_integrity(base,source,output).reason==playback_integrity(base,source,output).reason,"conclusion has no mutable global state");
    std::cout<<"PASS checks="<<checks<<" devices=0\n";return 0;
}catch(const std::exception&e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<"\n";return 1;}}
