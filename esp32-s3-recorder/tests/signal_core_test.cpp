#include "../firmware/MC2_Live/signal_core.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace mc2live;
static unsigned checks = 0;
static void check(bool value, const char* message) {
  ++checks; if (!value) { std::fprintf(stderr,"FAIL: %s\n",message); std::exit(1); }
}
static int16_t pcm[FFT_N], shifted[FFT_N];
static uint16_t raw[FFT_N];
static SpectrumProcessor fft;
int main() {
  check(frequencyBand(99.f)==-1 && frequencyBand(8001.f)==-1,"outside-band rejected");
  check(frequencyBand(100.f)==0 && frequencyBand(8000.f)==15,"inclusive band limits");
  for (size_t i=1;i<=BANDS;++i) check(bandEdge(i)>bandEdge(i-1),"monotone frequency edges");
  for(size_t mode=0;mode<4;++mode) {
    auto stats=fillDemo(pcm,mode,0);
    const auto s=fft.analyse(pcm);
    check(stats.source==Source::DEMO && !stats.adcValid,"DEMO never claims ADC provenance");
    check(std::fabs(s.peakHz-DEMO_TONES[mode])<=SAMPLE_RATE/float(FFT_N),"generated tone measured in correct FFT bin");
    check(s.peakBand==frequencyBand(DEMO_TONES[mode]),"tone band matches frequency map");
    check(s.level[s.peakBand]>0.44f && s.level[s.peakBand]<0.46f,"Hann-normalised tone amplitude");
    auto p=frequencyPixels(s); bool active=false;
    for(size_t x=0;x<32;++x) for(size_t y=0;y<8;++y) active |= p.at(x,y);
    check(active,"frequency view emits measured tone pixels");
  }
  fillDemo(pcm,4,0); const auto dual=fft.analyse(pcm);
  check(dual.level[frequencyBand(750)]>0.22f && dual.level[frequencyBand(3000)]>0.22f,"two simultaneous tones occupy two bands");
  check(dual.level[frequencyBand(750)]<0.23f && dual.level[frequencyBand(3000)]<0.23f,"dual amplitudes preserved");
  fillTone(pcm,FFT_N,0,750,0.2f);
  for(size_t i=0;i<FFT_N;++i)shifted[i]=pcm[i]+11000;
  const auto base=fft.analyse(pcm), dc=fft.analyse(shifted);
  check(base.peakBin==dc.peakBin,"DC offset does not change peak frequency");
  check(std::fabs(base.level[base.peakBand]-dc.level[dc.peakBand])<0.00001f,"DC offset removed before FFT");
  std::fill_n(pcm,FFT_N,12000); const auto quiet=fft.analyse(pcm);
  check(quiet.peakBand==-1 && quiet.peakHz==0,"constant input has no false tone");
  for(float level:quiet.level)check(level==0,"constant input has no band energy");
  std::fill_n(raw,FFT_N,2100); auto rs=convertAdcFrame(raw,pcm);
  check(rs.adcValid && rs.source==Source::ADC,"raw ADC conversion provenance");
  check(rs.rawMin==2100 && rs.rawMax==2100 && rs.rawMean==2100 && rs.rawAcRms==0,"constant raw statistics");
  check(std::all_of(pcm,pcm+FFT_N,[](int16_t x){return x==0;}),"raw mean subtracted from TIME samples");
  raw[0]=4095; for(size_t i=1;i<FFT_N;++i)raw[i]=0;
  rs=convertAdcFrame(raw,pcm);
  check(pcm[0]>32740 && pcm[0]<=32760,"extreme skewed raw frame does not signed-overflow");
  check(rs.lowRail==FFT_N-1 && rs.highRail==1,"raw clipping counters count rails");
  check(rs.rawMin==0 && rs.rawMax==4095,"rail min and max preserved");
  check(!convertAdcFrame(nullptr,pcm).adcValid,"null ADC input not accepted");
  for(size_t i=0;i<FFT_N;++i) raw[i]=static_cast<uint16_t>(2000+std::lround(400*std::sin(2*DSP_PI*750*i/SAMPLE_RATE)));
  rs=convertAdcFrame(raw,pcm); const auto real=fft.analyse(pcm);
  check(real.peakHz==750.f,"raw ADC sine retains its frequency");
  check(std::fabs(real.level[real.peakBand]-400*ADC_COUNT_TO_PCM/32768.f)<0.001f,"raw ADC to signed PCM scaling is exact/documented");
  check(std::fabs(rs.rawAcRms-400/std::sqrt(2.f))<1,"raw RMS measured in ADC counts");
  auto t=timePixels(pcm); auto tf=timePixels(pcm,FFT_N,8.f);
  unsigned pixels=0,pixelsGain=0;
  for(size_t x=0;x<32;++x)for(size_t y=0;y<8;++y){pixels+=t.at(x,y);pixelsGain+=tf.at(x,y);}
  check(pixels>0 && pixelsGain>=pixels,"time renderer uses real PCM and configurable visual gain");
  const auto boosted=fft.analyse(pcm,FFT_N,8.f);
  check(boosted.peakHz==real.peakHz && boosted.level[boosted.peakBand]==real.level[real.peakBand],"visual gain does not modify measured frequency or amplitude");
  check(boosted.height[boosted.peakBand]>=real.height[real.peakBand],"visual gain affects only pixel height");
  auto partial=timePixels(pcm,8,1,false);
  check(partial.at(0,4)||partial.at(0,3),"partial time window uses available sample group");
  bool extra=false;for(size_t x=1;x<32;++x)for(size_t y=0;y<8;++y)extra|=partial.at(x,y);
  check(!extra,"missing time samples remain blank, not invented");
  std::printf("PASS %u signal-source / ADC-conversion / FFT / display checks\n",checks);
}
