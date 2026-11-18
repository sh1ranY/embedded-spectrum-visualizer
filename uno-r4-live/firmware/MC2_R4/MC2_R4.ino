#include <Arduino.h>
#include "live_displays.h"
#include "r4_sampler.h"
#include "signal_core.h"

// Real ADC is the boot default. This version does not record or save audio.
// Commands TEST / DEMO are explicitly labelled and are never microphone proof.
mc2hw::LiveDisplays displays;
mc2r4::Sampler sampler;
mc2live::SpectrumProcessor processor;
uint16_t rawFrame[mc2live::FFT_N];
int16_t pcmFrame[mc2live::FFT_N];
mc2live::FrameStats stats;
mc2live::Spectrum spectrum;
mc2live::View view = mc2live::View::FREQ;
bool holdDisplay = false, diagnosticPattern = false, demo = false;
bool adcStarted = false, haveFrame = false;
uint32_t processed = 0, nextLog = 0, nextLcd = 0, nextDemo = 0;
uint32_t previousSamples = 0, previousLogTime = 0, measuredRate = 0;
uint32_t maxDspUs = 0;
float visualGain = 16.0f;
char command[48];
uint8_t commandLength = 0;

bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
const char* modeName() { return mc2live::viewName(view); }
const char* sourceName() { return demo ? "DEMO" : "ADC"; }

void pattern() {
  static const uint8_t digits[4][5] = {
    {2,6,2,2,7}, {7,1,7,4,7}, {7,1,7,1,7}, {5,5,7,1,1}
  };
  mc2live::PixelFrame p;
  for (uint8_t m=0; m<4; ++m) {
    p.set(m*8,0); p.set(m*8+6,7); p.set(m*8+7,7);
    for (uint8_t y=0;y<5;++y) for (uint8_t x=0;x<3;++x)
      if (digits[m][y] & (4>>x)) p.set(m*8+x+2,y+1);
  }
  displays.showRows(p.rows);
}

void render() {
  if (diagnosticPattern) { pattern(); return; }
  if (!haveFrame) return;
  const mc2live::PixelFrame pixels = view == mc2live::View::FREQ
      ? mc2live::frequencyPixels(spectrum)
      : mc2live::timePixels(pcmFrame, mc2live::FFT_N, visualGain);
  displays.showRows(pixels.rows);
}

void updateText() {
  char line1[24], line2[32];
  if (diagnosticPattern) {
    displays.setText("R4 DISPLAY TEST", "MATRIX 1 2 3 4"); return;
  }
  snprintf(line1,sizeof(line1),"%s %s %s",sourceName(),holdDisplay?"HOLD":"LIVE",modeName());
  if (!demo && (!adcStarted || sampler.fault()))
    snprintf(line2,sizeof(line2),"ADC ERROR %u",sampler.errorCode());
  else if (!haveFrame) snprintf(line2,sizeof(line2),"WAITING A0");
  else if (!demo && (stats.lowRail > mc2live::FFT_N/2 || stats.highRail > mc2live::FFT_N/2))
    snprintf(line2,sizeof(line2),"INPUT RAIL %u",unsigned(stats.rawMean));
  else if (view == mc2live::View::FREQ)
    snprintf(line2,sizeof(line2),"%4uHz P%4u",unsigned(spectrum.peakHz+0.5f),stats.rawMax-stats.rawMin);
  else snprintf(line2,sizeof(line2),"6.4ms P%4u x16",stats.rawMax-stats.rawMin);
  displays.setText(line1,line2);
}

void analyse() {
  const uint32_t started = micros();
  spectrum = processor.analyse(pcmFrame,mc2live::FFT_N,visualGain);
  // Do not present an arbitrary noise-bin as a measured tone in near silence.
  if (!demo && stats.rawAcRms < 2.0f) {
    spectrum.peakHz=0; spectrum.peakBand=-1;
  }
  const uint32_t elapsed=micros()-started;
  if (elapsed>maxDspUs) maxDspUs=elapsed;
  haveFrame=true; ++processed;
  render();
}

void printStatus() {
  const uint32_t now=millis(), samples=sampler.totalSamples();
  if (previousLogTime && now!=previousLogTime)
    measuredRate=uint32_t(uint64_t(samples-previousSamples)*1000/(now-previousLogTime));
  previousLogTime=now; previousSamples=samples;
  char line[600];
  snprintf(line,sizeof(line),
    "{\"v\":\"R4-0.1\",\"ms\":%lu,\"src\":\"%s\",\"view\":\"%s\",\"hold\":%u,\"test\":%u,"
    "\"lcd\":%u,\"i2cerr\":%lu,\"green\":%d,\"red\":%d,\"gp\":%lu,\"rp\":%lu,"
    "\"adc\":%u,\"fault\":%u,\"err\":%u,\"fsp\":%lu,\"fs\":%lu,\"hzActual\":%lu,"
    "\"n\":%lu,\"drop\":%lu,\"busy\":%lu,\"late\":%lu,\"dtMin\":%lu,\"dtMax\":%lu,"
    "\"min\":%u,\"max\":%u,\"mean\":%.1f,\"rms\":%.2f,\"peak\":%.1f,\"dspUs\":%lu}",
    (unsigned long)now,sourceName(),modeName(),holdDisplay,diagnosticPattern,
    displays.lcdReady(),(unsigned long)displays.i2cErrors(),displays.greenLevel(),displays.redLevel(),
    (unsigned long)displays.greenPresses(),(unsigned long)displays.redPresses(),
    adcStarted,sampler.fault(),sampler.errorCode(),(unsigned long)sampler.lastFspError(),
    (unsigned long)sampler.sampleRate(),(unsigned long)measuredRate,(unsigned long)samples,
    (unsigned long)sampler.overruns(),(unsigned long)sampler.adcBusyCount(),(unsigned long)sampler.lateTicks(),
    (unsigned long)sampler.minIntervalUs(),(unsigned long)sampler.maxIntervalUs(),
    stats.rawMin,stats.rawMax,stats.rawMean,stats.rawAcRms,spectrum.peakHz,(unsigned long)maxDspUs);
  Serial.println(line);
}

void execute(const char* cmd) {
  if (!strcmp(cmd,"STATUS")) { printStatus(); return; }
  if (!strcmp(cmd,"TEST")) { diagnosticPattern=true; holdDisplay=false; demo=false; pattern(); }
  else if (!strcmp(cmd,"DEMO")) { demo=true; diagnosticPattern=false; holdDisplay=false; haveFrame=false; }
  else if (!strcmp(cmd,"LIVE")) { demo=false; diagnosticPattern=false; holdDisplay=false; haveFrame=false; displays.clearMatrix(); }
  else if (!strcmp(cmd,"TIME")) { view=mc2live::View::TIME; diagnosticPattern=false; render(); }
  else if (!strcmp(cmd,"FREQ")) { view=mc2live::View::FREQ; diagnosticPattern=false; render(); }
  else if (!strcmp(cmd,"HOLD")) holdDisplay=true;
  else if (!strcmp(cmd,"RUN")) holdDisplay=false;
  else if (!strcmp(cmd,"LCD")) displays.retryLcd();
  else { Serial.println("COMMANDS STATUS TEST DEMO LIVE TIME FREQ HOLD RUN LCD"); return; }
  updateText();
  Serial.print("CMD "); Serial.println(cmd);
}

void setup() {
  Serial.begin(115200); // On R4 WiFi NO_USB this is the onboard ESP bridge UART.
  displays.begin();
  displays.wakeMatrix();
  adcStarted=sampler.begin();
  nextLog=millis()+1000;
  Serial.println("MC2 R4-0.1 | REAL A0 | RED TIME/FREQ | GREEN HOLD/LIVE | NO RECORDING");
  updateText();
}

void loop() {
  const uint32_t now=millis();
  displays.tick(now);
  const auto buttons=displays.takeButtons();
  if (buttons.redPressed) {
    view=view==mc2live::View::FREQ?mc2live::View::TIME:mc2live::View::FREQ;
    diagnosticPattern=false; render(); updateText();
    Serial.print("BUTTON RED "); Serial.println(modeName());
  }
  if (buttons.greenPressed) {
    holdDisplay=!holdDisplay; updateText();
    Serial.print("BUTTON GREEN "); Serial.println(holdDisplay?"HOLD":"LIVE");
  }
  while (Serial.available()) {
    const char c=Serial.read();
    if (c=='\r'||c=='\n') { if (commandLength) { command[commandLength]=0; execute(command); commandLength=0; } }
    else if (commandLength<sizeof(command)-1) command[commandLength++]=c;
    else commandLength=0;
  }
  const bool frameReady=sampler.takeFrame(rawFrame);
  if (demo && !holdDisplay && due(now,nextDemo)) {
    nextDemo=now+80;
    stats=mc2live::fillDemo(pcmFrame,1,uint64_t(processed)*mc2live::FFT_N);
    analyse();
  } else if (!demo && frameReady && !holdDisplay) {
    stats=mc2live::convertAdcFrame(rawFrame,pcmFrame);
    analyse();
  }
  if (due(now,nextLcd)) { nextLcd=now+400; updateText(); }
  if (due(now,nextLog)) { nextLog=now+1000; printStatus(); }
}
