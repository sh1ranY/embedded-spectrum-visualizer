/* ELEC281 MC2 v0.5 -- digital board recorder and saved-PCM visual review.
 * Default: explicitly labelled DEMO. No microphone input enabled at boot.
 * Same PCM frames feed the FFT/TIME display and PSRAM clip buffer; flash saving follows STOP.
 * No network, camera, speaker playback, partition change or autoformat.
 * GPIO unchanged from physically verified v0.4; CH343 UART USB only.
 */
#include <Arduino.h>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include "signal_core.h"
#include "audio_capture.h"
#include "live_displays.h"
#include "record_store.h"
#include "recorder_ui.h"

using namespace mc2live;
using mc2ui::Mode;
using mc2ui::Action;
static mc2hw::LiveDisplays hardware;
static AudioCapture acquisition;
static SpectrumProcessor processor;
static mc2record::RecordStore recordings;
static mc2ui::Controller controls;
static int16_t pcm[FFT_N] = {};
static Spectrum spectrum;
static FrameStats frameStats;
static Source source = Source::DEMO;
static View view = View::FREQ;
static constexpr int AUDIO_GPIO = 1; // Reserved; teammate circuit not connected.
static constexpr uint32_t FRAME_US = uint32_t(uint64_t(FFT_N) * 1000000 / SAMPLE_RATE);
static size_t demoMode = 1, selected = 0;
static uint64_t demoSample = 0, renderedFrames = 0, replaySamples = 0;
static uint64_t previousAdcFirst = 0;
static uint32_t adcDropAtStart = 0, adcOverflowAtStart = 0;
static bool havePreviousAdc = false, hasFrame = false, digitalReady = false;
static bool reviewEnd = false, deleteAllSelected = false;
static float visualGain = 1.f;
static uint32_t nextFrameUs = 0, nextReplayUs = 0, heartbeatAt = 0, lastAdcFrameAt = 0;
static uint32_t toastUntil = 0, demoLateFrames = 0;
static char toast[17] = {}, faultLabel[17] = "INPUT OFF", deleteTarget[24] = {};
static char command[96] = {};
static size_t commandLength = 0;
static bool commandOverflow = false;

static bool due(uint32_t now, uint32_t deadline) { return int32_t(now - deadline) >= 0; }
static const char* modeName() {
  switch (controls.mode()) {
    case Mode::LIVE: return "LIVE"; case Mode::RECORDING: return "REC";
    case Mode::FINALIZING: return "SAVING"; case Mode::BROWSE: return "FILES";
    case Mode::REVIEW: return reviewEnd ? "END" : "PLAY";
    case Mode::DELETE_CONFIRM: return "DELETE?";
  }
  return "?";
}
static const char* entrySource(const mc2record::Entry& e) {
  return e.legacy ? "OLD" : e.demo ? "DEMO" : "MIC";
}
static const char* inputSource() { return source == Source::DEMO ? "DEMO" : source == Source::ADC ? "MIC" : "OFF"; }
static void fileId(const char* name, char* out, size_t bytes) {
  const char* start = strrchr(name, '/'); start = start ? start + 1 : name;
  snprintf(out, bytes, "%s", start);
  if (char* dot = strrchr(out, '.')) *dot = 0;
}
static void clearPcm() {
  memset(pcm, 0, sizeof(pcm)); spectrum = {}; frameStats = {}; hasFrame = false;
  hardware.clearMatrix();
}
static void notify(const char* label) {
  snprintf(toast, sizeof(toast), "%s", label); toastUntil = millis() + 2500;
  Serial0.printf("NOTICE %s\n", label);
}
static void updateLcd() {
  char first[48] = {}, second[48] = {}, id[24] = {};
  const Mode mode = controls.mode();
  if (mode == Mode::LIVE) {
    snprintf(first, sizeof(first), "%s LIVE %s", inputSource(), viewName(view));
    snprintf(second, sizeof(second), "%s", recordings.ready() ? "G:REC hold:FILES" : "STORAGE OFF");
    if (source == Source::NONE) snprintf(first, sizeof(first), "%s", faultLabel);
  } else if (mode == Mode::RECORDING || mode == Mode::FINALIZING) {
    fileId(recordings.activeName(), id, sizeof(id));
    snprintf(first, sizeof(first), "%s %s %s", id, mode == Mode::RECORDING ? "REC" : "SAVE", viewName(view));
    snprintf(second, sizeof(second), "%s %.1fs%s", inputSource(), float(mode == Mode::RECORDING ? recordings.acceptedSamples() : recordings.writtenSamples()) / SAMPLE_RATE,
             mode == Mode::RECORDING ? " G:STOP" : "");
  } else if (mode == Mode::DELETE_CONFIRM) {
    snprintf(first, sizeof(first), deleteAllSelected ? "DELETE ALL AUDIO?" : "DEL %s?", deleteTarget);
    snprintf(second, sizeof(second), "G:YES R:NO");
  } else if (recordings.count() == 0) {
    snprintf(first, sizeof(first), "FILES EMPTY 0/0"); snprintf(second, sizeof(second), "G hold:LIVE");
  } else if (selected >= recordings.count()) {
    snprintf(first, sizeof(first), "ALL FILES %02u/%02u", unsigned(selected + 1), unsigned(recordings.count() + 1));
    snprintf(second, sizeof(second), "Hold both:DELETE");
  } else {
    const auto& entry = recordings.entry(selected); fileId(entry.name, id, sizeof(id));
    if (mode == Mode::REVIEW) {
      snprintf(first, sizeof(first), "%s %s %s", id, reviewEnd ? "END" : "PLAY", viewName(view));
      snprintf(second, sizeof(second), "%s %04.1f/%04.1fs", entrySource(entry), float(replaySamples) / SAMPLE_RATE,
               float(entry.samples) / SAMPLE_RATE);
    } else {
      snprintf(first, sizeof(first), "%s %02u/%02u%s", id, unsigned(selected + 1), unsigned(recordings.count()), entry.partial ? "*" : "");
      snprintf(second, sizeof(second), "%s %04.1fs %s", entrySource(entry), float(entry.samples) / SAMPLE_RATE,
               entry.valid ? viewName(view) : "BAD");
    }
  }
  if (toast[0] && !due(millis(), toastUntil)) snprintf(second, sizeof(second), "%s", toast);
  hardware.setText(first, second);
}
static void renderFrame() {
  if (!digitalReady || !hasFrame) return;
  const PixelFrame pixels = view == View::FREQ ? frequencyPixels(spectrum) : timePixels(pcm, FFT_N, visualGain);
  hardware.showRows(pixels.rows); ++renderedFrames;
}
static void analyseFrame() { spectrum = processor.analyse(pcm, FFT_N, visualGain); hasFrame = true; renderFrame(); }
static void setView(View next) {
  view = next; renderFrame(); updateLcd();
  Serial0.printf("VIEW %s mode=%s input=%s\n", viewName(view), modeName(), inputSource());
}
static bool isLiveScreen() { return controls.mode() == Mode::LIVE; }
static void listFiles() {
  Serial0.printf("FILES count=%u total=%u used=%u free=%u ready=%u\n", unsigned(recordings.count()),
    unsigned(recordings.storageTotalBytes()), unsigned(recordings.storageUsedBytes()), unsigned(recordings.freeBytes()), unsigned(recordings.ready()));
  for (size_t i = 0; i < recordings.count(); ++i) {
    const auto& e = recordings.entry(i);
    Serial0.printf("FILE index=%u name=%s source=%s samples=%lu seconds=%.4f valid=%u partial=%u\n", unsigned(i), e.name,
      entrySource(e), (unsigned long)e.samples, float(e.samples) / SAMPLE_RATE, unsigned(e.valid), unsigned(e.partial));
  }
}
static void status() {
  Serial0.printf("STATUS uptime_ms=%lu input=%s mode=%s view=%s demo=%u frames=%llu peak_hz=%.2f gain=%.1f fs=%u files=%u selected=%u record=%u busy=%u saved_samples=%lu recorded_samples=%lu replay_samples=%llu reason=%s lcd_ready=%u i2c_err=%lu adc_running=%u adc_samples=%llu adc_drops=%lu adc_overflow=%lu demo_late=%lu free_heap=%u\n",
    (unsigned long)millis(), inputSource(), modeName(), viewName(view), unsigned(demoMode), (unsigned long long)renderedFrames,
    spectrum.peakHz, visualGain, unsigned(recordings.ready()), unsigned(recordings.count()), unsigned(selected),
    unsigned(recordings.recording()), unsigned(recordings.busy()), (unsigned long)recordings.writtenSamples(),
    (unsigned long)recordings.acceptedSamples(), (unsigned long long)replaySamples, recordings.lastReason(), unsigned(hardware.lcdReady()), (unsigned long)hardware.i2cErrors(),
    unsigned(acquisition.running()), (unsigned long long)acquisition.samples(), (unsigned long)acquisition.droppedFrames(),
    (unsigned long)acquisition.poolOverflows(), (unsigned long)demoLateFrames, ESP.getFreeHeap());
}
static void scheduleLive() { nextFrameUs = micros(); lastAdcFrameAt = millis(); }
static void startDemo(size_t mode) {
  acquisition.stop(); source = Source::DEMO; demoMode = mode % DEMO_MODES;
  visualGain = 1; demoSample = 0; clearPcm(); hardware.wakeMatrix(); scheduleLive();
  Serial0.printf("SOURCE DEMO signal=%s generated_PCM_only\n", demoName(demoMode)); updateLcd();
}
static bool startAdc() {
  if (!acquisition.start(AUDIO_GPIO)) {
    source = Source::NONE; clearPcm(); snprintf(faultLabel, sizeof(faultLabel), "ADC START ERROR");
    notify("ADC START ERROR"); return false;
  }
  source = Source::ADC; visualGain = 16; scheduleLive(); havePreviousAdc = false;
  adcDropAtStart = acquisition.droppedFrames(); adcOverflowAtStart = acquisition.poolOverflows();
  clearPcm(); hardware.wakeMatrix();
  Serial0.println("SOURCE MIC ADC_GPIO1 operator_confirmed_hardware nominal48000sps; not_voltage_calibrated"); return true;
}
static void enterBrowser(bool chooseActive = false) {
  recordings.tick(); // Acquire a completed writer and refresh the file index before selection.
  acquisition.stop(); recordings.closeReview(); clearPcm(); reviewEnd = false; replaySamples = 0;
  controls.setMode(Mode::BROWSE);
  if (chooseActive) {
    for (size_t i = 0; i < recordings.count(); ++i)
      if (!strcmp(recordings.entry(i).name, recordings.activeName())) { selected = i; break; }
  }
  if (selected > recordings.count()) selected = 0;
  updateLcd(); Serial0.printf("BROWSER selected=%u count=%u\n", unsigned(selected), unsigned(recordings.count()));
}
static void exitBrowser() {
  recordings.closeReview(); clearPcm(); controls.setMode(Mode::LIVE); reviewEnd = false; replaySamples = 0;
  if (source == Source::ADC) startAdc(); else scheduleLive();
  updateLcd(); Serial0.println("BROWSER_EXIT replay_cache_cleared");
}
static void startRecord() {
  if (!isLiveScreen() || recordings.busy()) { notify("BUSY"); return; }
  if (source == Source::NONE) { notify("INPUT OFF"); return; }
  if (!recordings.start(source == Source::DEMO)) { notify(recordings.lastReason()); return; }
  if (source == Source::DEMO) { demoSample = 0; scheduleLive(); }
  // A discontinuity in the later ADC stream must stop, never concatenate gaps.
  adcDropAtStart = acquisition.droppedFrames(); adcOverflowAtStart = acquisition.poolOverflows(); havePreviousAdc = false;
  controls.setMode(Mode::RECORDING); toast[0] = 0;
  Serial0.printf("RECORD_START file=%s source=%s cap=10s nominal_sps=%lu PCM_shared_with_live_display\n", recordings.activeName(), inputSource(), (unsigned long)SAMPLE_RATE);
  updateLcd();
}
static void stopRecord(const char* reason = "STOPPED") {
  if (recordings.recording()) recordings.requestStop(reason);
  if (recordings.busy()) controls.setMode(Mode::FINALIZING);
  updateLcd();
}
static void nextFile() {
  recordings.closeReview(); clearPcm(); reviewEnd = false; replaySamples = 0;
  selected = recordings.count() ? (selected + 1) % (recordings.count() + 1) : 0;
  controls.setMode(Mode::BROWSE); updateLcd();
  Serial0.printf("FILE_SELECT index=%u all=%u\n", unsigned(selected), unsigned(recordings.count() && selected == recordings.count()));
}
static void startReview() {
  if (selected >= recordings.count()) { notify(recordings.count() ? "SELECT A FILE" : "NO RECORDINGS"); return; }
  recordings.closeReview(); clearPcm();
  if (!recordings.openReview(selected)) { notify("FILE NOT PLAYABLE"); return; }
  replaySamples = 0; reviewEnd = false; controls.setMode(Mode::REVIEW); nextReplayUs = micros();
  const auto& e = recordings.entry(selected);
  Serial0.printf("REVIEW_START file=%s source=%s samples=%lu visual_review_only_no_audio_output\n", e.name, entrySource(e), (unsigned long)e.samples);
  updateLcd();
}
static void requestDelete(bool all = false) {
  if (recordings.busy()) { notify("RECORD BUSY"); return; }
  if (!recordings.count()) { notify("NO RECORDINGS"); return; }
  deleteAllSelected = all || selected >= recordings.count();
  snprintf(deleteTarget, sizeof(deleteTarget), "%s", deleteAllSelected ? "ALL AUDIO" : recordings.entry(selected).name);
  recordings.closeReview(); clearPcm(); controls.setMode(Mode::DELETE_CONFIRM);
  Serial0.printf("DELETE_PROMPT target=%s release_keys_then_G_confirm_R_cancel\n", deleteTarget); updateLcd();
}
static void deleteConfirmed() {
  // Filename was captured at the prompt; do not let navigation retarget deletion.
  recordings.closeReview(); clearPcm();
  bool ok = false;
  if (deleteAllSelected) ok = recordings.deleteAll();
  else for (size_t i = 0; i < recordings.count(); ++i) {
    if (!strcmp(recordings.entry(i).name, deleteTarget)) { ok = recordings.deleteAt(i); break; }
  }
  Serial0.printf("DELETE_RESULT target=%s success=%u files_remaining=%u cached_PCM_cleared=1\n", deleteTarget, unsigned(ok), unsigned(recordings.count()));
  selected = selected < recordings.count() ? selected : 0;
  controls.setMode(Mode::BROWSE); reviewEnd = false; replaySamples = 0;
  notify(ok ? "DELETED" : "DELETE FAILED"); updateLcd();
}
static void handleAction(Action a) {
  switch (a) {
    case Action::NONE: break;
    case Action::RECORD_TOGGLE: if (recordings.recording()) stopRecord(); else startRecord(); break;
    case Action::TOGGLE_VIEW: setView(view == View::FREQ ? View::TIME : View::FREQ); break;
    case Action::ENTER_BROWSER: enterBrowser(); break;
    case Action::EXIT_BROWSER: exitBrowser(); break;
    case Action::NEXT_FILE: nextFile(); break;
    case Action::START_REVIEW: startReview(); break;
    case Action::REQUEST_DELETE: requestDelete(); break;
    case Action::CONFIRM_DELETE: deleteConfirmed(); break;
    case Action::CANCEL_DELETE: controls.setMode(Mode::BROWSE); notify("DELETE CANCELLED"); updateLcd(); break;
  }
}
static void dspSelftest() {
  if (!isLiveScreen() || source != Source::DEMO) { notify("SELFTEST LIVE DEMO"); return; }
  unsigned failures = 0;
  for (size_t m = 0; m < DEMO_MODES; ++m) {
    fillDemo(pcm, m, 0); const auto s = processor.analyse(pcm);
    const bool ok = m < 4 ? std::fabs(s.peakHz - DEMO_TONES[m]) < float(SAMPLE_RATE) / FFT_N
                          : s.level[frequencyBand(750)] > .2f && s.level[frequencyBand(3000)] > .2f;
    failures += !ok; Serial0.printf("DSP_TEST mode=%u result=%s\n", unsigned(m), ok ? "PASS" : "FAIL");
  }
  Serial0.printf("DSP_DONE failures=%u scope=generated_PCM_only\n", failures); clearPcm(); scheduleLive();
}
static void help() {
  Serial0.println("MC2v0.5: green tap REC/STOP; live green hold1.2s FILES; files green tap NEXT including ALL FILES, hold1.2s LIVE.");
  Serial0.println("Red tap TIME/FREQ during LIVE/REC/FILES/PLAY; files red hold1.2s PLAY/REPLAY; both hold1.5s DELETE prompt; release then G=yes/R=no.");
  Serial0.println("UART STATUS|LIST|REC|STOP|FILES|NEXT|PLAY|LIVE|TIME|FREQ|DELETE|CONFIRM|CANCEL|DEMO 0..4|GAIN 1..64|SELFTEST|LCD RETRY|OFF");
  Serial0.println("STORAGE INIT BLANK only accepts wholly erased partition; never autoformat. ADC CONFIRM reserved until teammate GPIO1/range/commonGND verified.");
}
static void handleCommand(char* c) {
  if (!strcmp(c, "STATUS")) status();
  else if (!strcmp(c, "HELP")) help();
  else if (!strcmp(c, "LIST")) listFiles();
  else if (!strcmp(c, "TIME")) setView(View::TIME);
  else if (!strcmp(c, "FREQ")) setView(View::FREQ);
  else if (!strcmp(c, "REC")) startRecord();
  else if (!strcmp(c, "STOP")) { if (recordings.recording()) stopRecord(); }
  else if (!strcmp(c, "FILES")) {
    if (!recordings.busy()) enterBrowser(); else notify("BUSY");
  } else if (!strcmp(c, "LIVE")) {
    if (!recordings.busy()) exitBrowser(); else notify("BUSY");
  } else if (!strcmp(c, "NEXT")) {
    if (controls.mode() == Mode::BROWSE || controls.mode() == Mode::REVIEW) nextFile(); else notify("OPEN FILES FIRST");
  } else if (!strcmp(c, "PLAY")) {
    if (controls.mode() == Mode::BROWSE || controls.mode() == Mode::REVIEW) startReview(); else notify("OPEN FILES FIRST");
  } else if (!strcmp(c, "DELETE")) {
    if (controls.mode() == Mode::BROWSE || controls.mode() == Mode::REVIEW) requestDelete(); else notify("OPEN FILES FIRST");
  } else if (!strcmp(c, "CONFIRM")) {
    if (controls.mode() == Mode::DELETE_CONFIRM) deleteConfirmed();
  } else if (!strcmp(c, "CANCEL")) {
    if (controls.mode() == Mode::DELETE_CONFIRM) handleAction(Action::CANCEL_DELETE);
  } else if (!strcmp(c, "STORAGE INIT BLANK")) {
    if (isLiveScreen() && !recordings.busy() && !recordings.ready()) {
      const bool ok = recordings.formatBlankPartition(); notify(ok ? "STORAGE READY" : "INIT REFUSED"); listFiles();
    } else notify("INIT NOT NEEDED");
  } else if (!strcmp(c, "DEMO") || !strncmp(c, "DEMO ", 5)) {
    if (!isLiveScreen()) { notify("RETURN LIVE FIRST"); return; }
    char* end; const long m = strlen(c) == 4 ? long(demoMode) : strtol(c + 5, &end, 10);
    if (strlen(c) == 4 || (*end == 0 && m >= 0 && m < long(DEMO_MODES))) startDemo(size_t(m));
    else notify("DEMO NEEDS 0..4");
  } else if (!strncmp(c, "GAIN ", 5)) {
    char* end; const float g = strtof(c + 5, &end);
    if (*end == 0 && std::isfinite(g) && g >= 1 && g <= 64) { visualGain = g; if (hasFrame) analyseFrame(); }
    else notify("GAIN NEEDS 1..64");
  } else if (!strcmp(c, "ADC CONFIRM")) {
    if (isLiveScreen()) startAdc(); else notify("RETURN LIVE FIRST");
  } else if (!strcmp(c, "SELFTEST")) dspSelftest();
  else if (!strcmp(c, "LCD RETRY")) hardware.retryLcd();
  else if (!strcmp(c, "OFF")) {
    if (recordings.busy()) { notify("STOP REC FIRST"); return; }
    recordings.closeReview(); acquisition.stop(); source = Source::NONE; clearPcm(); controls.setMode(Mode::LIVE);
  } else Serial0.println("UNKNOWN_COMMAND use_HELP");
  updateLcd();
}
static void serialTick() {
  unsigned budget = 128;
  while (budget-- && Serial0.available()) {
    const char c = char(Serial0.read());
    if (c == '\r' || c == '\n') {
      if (commandOverflow) Serial0.println("REJECT command_too_long");
      else if (commandLength) { command[commandLength] = 0; handleCommand(command); }
      commandLength = 0; commandOverflow = false;
    } else if (c >= 32 && c <= 126) {
      if (commandLength + 1 < sizeof(command)) command[commandLength++] = c; else commandOverflow = true;
    }
  }
}
static void acceptLiveFrame() {
  if (controls.mode() == Mode::RECORDING && recordings.recording()) {
    if (source == Source::ADC &&
        (acquisition.droppedFrames() != adcDropAtStart || acquisition.poolOverflows() != adcOverflowAtStart ||
         (havePreviousAdc && frameStats.firstSampleIndex != previousAdcFirst + FFT_N))) {
      stopRecord("INPUT GAP"); notify("REC INPUT GAP");
    } else if (!recordings.enqueue(pcm, FFT_N)) stopRecord(recordings.lastReason());
    previousAdcFirst = frameStats.firstSampleIndex; havePreviousAdc = true;
  }
  analyseFrame();
}
static void liveTick() {
  const uint32_t nowUs = micros();
  if (source == Source::DEMO && due(nowUs, nextFrameUs)) {
    if (uint32_t(nowUs - nextFrameUs) > 2 * FRAME_US) {
      ++demoLateFrames;
      if (recordings.recording()) stopRecord("TIMING GAP");
      nextFrameUs = nowUs;
    }
    frameStats = fillDemo(pcm, demoMode, demoSample); demoSample += FFT_N;
    nextFrameUs += FRAME_US; acceptLiveFrame();
  } else if (source == Source::ADC) {
    acquisition.poll();
    if (!acquisition.running() || uint32_t(millis() - lastAdcFrameAt) > 1500) {
      if (recordings.recording()) stopRecord("INPUT ERROR");
      acquisition.stop(); source = Source::NONE; snprintf(faultLabel, sizeof(faultLabel), "ADC ERROR"); clearPcm(); notify("INPUT ERROR");
    } else if (acquisition.popFrame(pcm, frameStats)) { lastAdcFrameAt = millis(); acceptLiveFrame(); }
  }
}
static void replayTick() {
  if (reviewEnd || !due(micros(), nextReplayUs)) return;
  if (replaySamples >= recordings.entry(selected).samples) {
    reviewEnd = true; recordings.closeReview(); // Keep the last valid PCM and spectrum coherent at END.
    Serial0.printf("REVIEW_END samples=%llu expected=%lu\n", (unsigned long long)replaySamples, (unsigned long)recordings.entry(selected).samples);
    updateLcd(); return;
  }
  const size_t n = recordings.readReviewFrame(pcm, FFT_N);
  if (!n) {
    Serial0.printf("REVIEW_ERROR samples=%llu expected=%lu reason=%s\n", (unsigned long long)replaySamples,
      (unsigned long)recordings.entry(selected).samples, recordings.lastReason());
    recordings.closeReview(); clearPcm(); controls.setMode(Mode::BROWSE); notify("READ FAILED"); return;
  }
  replaySamples += n; analyseFrame();
  // Pace by the amount of saved PCM, not FFT speed; avoid racing to EOF.
  const uint32_t interval = uint32_t(uint64_t(n) * 1000000 / SAMPLE_RATE);
  nextReplayUs += interval;
  if (int32_t(micros() - nextReplayUs) > int32_t(2 * FRAME_US)) nextReplayUs = micros() + interval;
}
void setup() {
  Serial0.begin(115200, SERIAL_8N1, 44, 43);
  Serial0.println("\nELEC281 MC2 v0.5 RECORD/BROWSE/REVIEW/DELETE | DEMO DEFAULT | 2026-11-05");
  Serial0.printf("BOOT flash=%u IDF=%s\n", ESP.getFlashChipSize(), ESP.getSdkVersion());
#ifdef BOARD_HAS_PSRAM
  Serial0.printf("PSRAM size=%u free=%u\n", ESP.getPsramSize(), ESP.getFreePsram());
#endif
  digitalReady = hardware.begin();
  const bool mounted = recordings.begin();
  Serial0.printf("STORAGE ready=%u reason=%s autoformat=0\n", unsigned(mounted), recordings.lastReason());
  if (digitalReady) startDemo(1); else { source = Source::NONE; snprintf(faultLabel, sizeof(faultLabel), "DIGITAL ERROR"); }
  help(); listFiles(); status(); heartbeatAt = millis() + 2000;
}
void loop() {
  serialTick(); hardware.tick(); hardware.takeButtons();
  recordings.tick();
  if (controls.mode() == Mode::RECORDING && !recordings.recording()) controls.setMode(Mode::FINALIZING);
  if (controls.mode() == Mode::FINALIZING && !recordings.busy()) {
    Serial0.printf("RECORD_DONE file=%s samples=%lu seconds=%.4f reason=%s\n", recordings.activeName(),
      (unsigned long)recordings.writtenSamples(), float(recordings.writtenSamples()) / SAMPLE_RATE, recordings.lastReason());
    enterBrowser(true); notify(recordings.lastReason());
  }
  if (hardware.buttonsArmed())
    handleAction(controls.tick(millis(), hardware.greenLevel() == 0, hardware.redLevel() == 0));
  const Mode mode = controls.mode();
  if (mode == Mode::LIVE || mode == Mode::RECORDING || mode == Mode::FINALIZING) liveTick();
  else if (mode == Mode::REVIEW) replayTick();
  updateLcd();
  const uint32_t now = millis(); if (due(now, heartbeatAt)) { status(); heartbeatAt = now + 2000; }
  delay(1);
}
