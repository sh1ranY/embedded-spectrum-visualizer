#include "../firmware/MC2_R4/signal_core.h"
#include <cassert>
#include <cstdio>
#include <limits>
#include <initializer_list>
using namespace mc2live;
static SpectrumProcessor processor;
int main() {
  int16_t pcm[FFT_N]{};
  uint16_t raw[FFT_N]{};
  const float binHz = float(SAMPLE_RATE)/FFT_N;
  for (float hz : {100.f,750.f,3000.f,8000.f}) {
    fillTone(pcm, FFT_N, 0, hz);
    Spectrum s=processor.analyse(pcm,FFT_N,16);
    assert(s.peakBand>=0 && s.peakBand<16);
    assert(std::abs(s.peakHz-hz)<=binHz);
    auto f=frequencyPixels(s);
    size_t pixels=0;
    for (auto h:s.height) assert(h<=8);
    for(size_t y=0;y<8;y++)for(size_t x=0;x<32;x++)pixels+=f.at(x,y);
    assert(pixels>0);
    auto w=timePixels(pcm,FFT_N,16);
    for(size_t x=0;x<32;x++){
      bool any=false;
      for(size_t y=0;y<8;y++) any|=w.at(x,y);
      assert(any);
    }
    std::printf("tone %.1f Hz -> %.3f Hz band %d pixels %zu\n",hz,s.peakHz,s.peakBand,pixels);
  }
  for (int16_t dc : {int16_t(-32768),int16_t(0),int16_t(16384),int16_t(32767)}) {
    for(auto &v:pcm)v=dc;
    auto s=processor.analyse(pcm);
    assert(s.peakBand==-1 && s.peakHz==0);
    for(auto l:s.level) assert(l==0);
    for(auto h:s.height) assert(h==0);
  }
  for(auto &v:raw)v=2048;
  auto stats=convertAdcFrame(raw,pcm);
  assert(stats.source==Source::ADC && stats.adcValid);
  assert(stats.rawMin==2048 && stats.rawMax==2048 && stats.rawAcRms==0);
  for(auto v:pcm)assert(v==0);
  for(size_t i=0;i<FFT_N;i++)raw[i]=i%2?4095:0;
  stats=convertAdcFrame(raw,pcm);
  assert(stats.rawMean==2047.5f && stats.lowRail==256 && stats.highRail==256);
  for(size_t i=0;i<FFT_N;i++)assert(pcm[i]==(i%2?16380:-16380));
  raw[0]=65535; for(size_t i=1;i<FFT_N;i++)raw[i]=0;
  stats=convertAdcFrame(raw,pcm);
  assert(stats.rawMax==4095 && pcm[0]>32000);
  stats=fillDemo(pcm,1,123);
  assert(stats.source==Source::DEMO && !stats.adcValid && stats.samples==512);
  assert(sampleRow(INT16_MIN,16)==7 && sampleRow(INT16_MAX,16)==0);
  assert(sampleRow(0,16)==4);
  auto empty=timePixels(nullptr);
  for(auto &r:empty.rows)for(auto v:r)assert(v==0);
  auto badgain=timePixels(pcm,FFT_N,std::numeric_limits<float>::quiet_NaN());
  (void)badgain;
  std::puts("PASS DSP frequency, DC removal, source provenance, ADC bounds and time pixels");
}
