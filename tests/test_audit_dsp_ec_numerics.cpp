// Calibration of the actual family-test estimator; no device or production SDM changes.
#define main original_modulator_families_main
#include "test_modulator_families.cpp"
#undef main
#include <stdexcept>
#include <limits>
static int auditChecks=0;
static void auditCheck(bool ok,const char*why){++auditChecks;if(!ok)throw std::runtime_error(why);}
static double powerDb(double p){return 10*std::log10(p);}
static Spec independentDft(const std::vector<double>& y,double fs) {
#if defined(__clang__)
#pragma clang fp reassociate(off)
#pragma clang fp contract(off)
#endif
    const size_t n=y.size(),top=(size_t)std::llround(20000.0/fs*n);
    double sig=0,noise=0,w2=0;
    for(size_t i=0;i<n;++i){double w=(1-std::cos(2*kPi*i/n))/2;w2+=w*w;}
    for(size_t k=1;k<=top;++k){double re=0,im=0,rc=0,ic=0;
        for(size_t i=0;i<n;++i){const double w=(1-std::cos(2*kPi*i/n))/2,angle=-2*kPi*k*i/n;
            const double a=y[i]*w*std::cos(angle)-rc,t=re+a;rc=(t-re)-a;re=t;
            const double b=y[i]*w*std::sin(angle)-ic,u=im+b;ic=(u-im)-b;im=u;}
        double p=2*(re*re+im*im)/(n*w2);if(k>=22&&k<=24)sig+=p;else noise+=p;}
    Spec s;s.signalDb=powerDb(sig);s.inBandNoiseDb=powerDb(noise);s.snrDb=powerDb(sig/noise);return s;
}
static double rectangularSnr(const std::vector<double>& y,double fs){const size_t n=y.size();std::vector<cplx>b(n);for(size_t i=0;i<n;++i)b[i]=y[i];fft(b);
    double signal=0,noise=0;for(size_t k=1;k<=(size_t)std::llround(20000/fs*n);++k){if(k>=22&&k<=24)signal+=std::norm(b[k]);else noise+=std::norm(b[k]);}return powerDb(signal/noise);}
int main(){try{
    const size_t n=65536,warm=20000;const double fs=22579200,fin=fs/n*23,amp=.5;
    double w1=0,w2=0;for(size_t i=0;i<n;++i){double w=.5-.5*std::cos(2*kPi*i/n);w1+=w;w2+=w*w;}
    auditCheck(std::abs(w1/n-.5)<1e-12&&std::abs(w2/n-.375)<1e-12,"Hann coherent gain and mean-square normalization");
    auditCheck(std::abs(n*w2/(w1*w1)-1.5)<1e-12,"Hann ENBW is 1.5 bins");
    std::vector<double> two(n),ultrasonic(n);for(size_t i=0;i<n;++i){const double fundamental=amp*std::sin(2*kPi*23*i/n+.37);
        two[i]=fundamental+1e-4*std::sin(2*kPi*10*i/n+.8);
        ultrasonic[i]=fundamental+.5*std::sin(2*kPi*(.4*n+.37)*i/n+1.2);}
    auto measured=analyse_spectrum(two,fs,fin),reference=independentDft(two,fs);
    const double expectedSnr=20*std::log10(amp/1e-4);
    auditCheck(std::abs(measured.signalDb-powerDb(amp*amp/2))<1e-6,"calibrated fundamental power");
    auditCheck(std::abs(measured.inBandNoiseDb-powerDb(1e-8/2))<1e-6,"-80 dBFS in-band contaminant power is retained");
    auditCheck(std::abs(measured.snrDb-expectedSnr)<1e-6&&measured.snrDb<105,"known in-band contamination still fails unchanged 105 dB gate");
    auditCheck(std::abs(measured.snrDb-reference.snrDb)<1e-6,"direct DFT independently agrees on physical SNR");
    auto filtered=analyse_spectrum(ultrasonic,fs,fin);const double old=rectangularSnr(ultrasonic,fs);
    auditCheck(old<105&&filtered.snrDb>130,"out-of-band-only contamination proves rectangular-boundary leakage");
    printf("calibration: tone=%.9f dBFS noise=%.9f dBFS SNR=%.9f direct=%.9f ENBW=1.5\n",measured.signalDb,measured.inBandNoiseDb,measured.snrDb,reference.snrDb);
    printf("ultrasonic-only: rectangular=%.9f Hann=%.9f dB\n",old,filtered.snrDb);
    // White-noise calibration explicitly accounts for the excluded three tone bins.
    double noiseSum=0;const double sigma=1e-4;const size_t bins=(size_t)std::llround(20000/fs*n)-3;
    for(uint64_t seed=1;seed<=32;++seed){uint64_t rng=seed*0x9e3779b97f4a7c15ull;std::vector<double> y(n);
        for(size_t i=0;i<n;++i){rng^=rng>>12;rng^=rng<<25;rng^=rng>>27;double uniform=(double)((rng*2685821657736338717ull)>>11)/9007199254740992.0-.5;
            y[i]=amp*std::sin(2*kPi*23*i/n)+sigma*std::sqrt(12.)*uniform;}
        auto stats=analyse_spectrum(y,fs,fin);noiseSum+=std::pow(10.,stats.inBandNoiseDb/10.);}
    const double expectedNoise=sigma*sigma*2*bins/n,whiteError=powerDb(noiseSum/32/expectedNoise);
    printf("white-noise: measured=%.9f expected=%.9f dBFS error=%.6f dB bins=%zu realizations=32\n",powerDb(noiseSum/32),powerDb(expectedNoise),whiteError,bins);
    auditCheck(std::abs(whiteError)<.5,"window power normalization retains calibrated broadband noise density");
    double minimum=1e9,maximum=-1e9;
    for(int phase=0;phase<8;++phase){SdmConfig cfg;cfg.fs=fs;cfg.order=7;cfg.family=ModulatorFamily::Ec;cfg.ditherLsb=.0005;SdmModulator mod(cfg);std::vector<double> y;y.reserve(n);
        for(size_t i=0;i<warm+n;++i){double value=mod.process(.5011872336*std::sin(2*kPi*fin*i/fs+phase*.37));if(i>=warm)y.push_back(value);}
        auto stats=analyse_spectrum(y,fs,fin);double rect=rectangularSnr(y,fs);minimum=std::min(minimum,stats.snrDb);maximum=std::max(maximum,stats.snrDb);
        printf("phase=%.3f rectangular=%.9f Hann=%.9f dB clamp=%lld overload=%lld\n",phase*.37,rect,stats.snrDb,mod.clamped(),mod.overload());
        auditCheck(stats.snrDb>105&&mod.clamped()==0&&mod.overload()==0,"actual same bitstream meets unchanged EC7 physical quality and stability gate");}
    printf("PASS checks=%d EC7 Hann range=%.9f..%.9f dB devices=0\n",auditChecks,minimum,maximum);return 0;
}catch(const std::exception&e){fprintf(stderr,"FAIL after %d: %s\n",auditChecks,e.what());return 1;}}
