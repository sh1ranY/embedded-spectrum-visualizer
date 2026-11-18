#pragma once
// ESP-IDF 5.5.5 public ADC continuous API (Arduino-ESP32 3.3.12).
// Constructor does NOT configure or sample a pin. Call start() only after the
// physical input/pin/voltage has been confirmed. Default candidate is GPIO1.
#include "signal_core.h"
#include "esp_adc/adc_continuous.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "soc/soc_caps.h"

namespace mc2live {
class AudioCapture {
 public:
  AudioCapture() = default;
  AudioCapture(const AudioCapture&) = delete;
  AudioCapture& operator=(const AudioCapture&) = delete;

  bool start(int gpio = 1) {
    stop(); error_ = ESP_OK;
    adc_unit_t unit; adc_channel_t channel;
    error_ = adc_continuous_io_to_channel(gpio, &unit, &channel);
    if (error_ != ESP_OK || unit != ADC_UNIT_1) { error_ = ESP_ERR_INVALID_ARG; return false; }
    adc_continuous_handle_cfg_t h = {};
    h.max_store_buf_size = 32768; h.conv_frame_size = 2048;
    h.flags.flush_pool = 0;
    if ((error_ = adc_continuous_new_handle(&h, &handle_)) != ESP_OK) return false;
    adc_digi_pattern_config_t pattern = {};
    pattern.atten = ADC_ATTEN_DB_12; pattern.channel = channel;
    pattern.unit = unit; pattern.bit_width = 12;
    adc_continuous_config_t c = {};
    c.pattern_num = 1; c.adc_pattern = &pattern; c.sample_freq_hz = SAMPLE_RATE;
    c.conv_mode = ADC_CONV_SINGLE_UNIT_1; c.format = ADC_DIGI_OUTPUT_FORMAT_TYPE2;
    if ((error_ = adc_continuous_config(handle_, &c)) != ESP_OK) return failStart();
    adc_continuous_evt_cbs_t callbacks = {};
    callbacks.on_pool_ovf = overflowCallback;
    if ((error_ = adc_continuous_register_event_callbacks(handle_, &callbacks, this)) != ESP_OK) return failStart();
    channel_ = channel; gpio_ = gpio; resetAssembly();
    samples_ = frames_ = droppedFrames_ = resyncs_ = invalidResults_ = 0;
    overflows_ = observedOverflows_ = 0;
    if ((error_ = adc_continuous_start(handle_)) != ESP_OK) return failStart();
    running_ = true; startUs_ = esp_timer_get_time(); return true;
  }
  void stop() {
    if (handle_) {
      if (running_) adc_continuous_stop(handle_);
      adc_continuous_deinit(handle_); handle_ = nullptr;
    }
    running_ = false; resetAssembly();
  }
  // Every read uses timeout=0. Drain at most16 DMA frames and approximately2ms
  // of CPU per call; invoke frequently from loop, not from an ISR.
  void poll() {
    if (!running_) return;
    if (overflows_ != observedOverflows_) { observedOverflows_ = overflows_; resync(); return; }
    const int64_t budget = esp_timer_get_time() + 2000;
    for (unsigned read = 0; read < 16; ++read) {
      uint32_t length = 0;
      const esp_err_t e = adc_continuous_read(handle_, dma_, sizeof(dma_), &length, 0);
      if (e == ESP_ERR_TIMEOUT) return;
      if (e != ESP_OK) { error_ = e; if (e == ESP_ERR_INVALID_STATE) resync(); else stop(); return; }
      for (uint32_t i = 0; i + SOC_ADC_DIGI_RESULT_BYTES <= length; i += SOC_ADC_DIGI_RESULT_BYTES) {
        const adc_digi_output_data_t* v = reinterpret_cast<const adc_digi_output_data_t*>(dma_ + i);
        if (v->type2.unit != 0 || v->type2.channel != channel_) { ++invalidResults_; continue; }
        raw_[filling_][filled_++] = v->type2.data; ++samples_;
        if (filled_ == FFT_N) {
          ++frames_;
          if (ready_ >= 0) ++droppedFrames_;
          ready_ = filling_; readySequence_ = frames_; readyFirstSample_ = samples_ - FFT_N;
          filling_ ^= 1; filled_ = 0;
        }
      }
      if (esp_timer_get_time() >= budget) return;
    }
  }
  bool frameReady() const { return ready_ >= 0; }
  bool popFrame(int16_t* pcm, FrameStats& stats) {
    if (ready_ < 0 || !pcm) return false;
    const int index = ready_;
    stats = convertAdcFrame(raw_[index], pcm);
    stats.sequence = readySequence_; stats.firstSampleIndex = readyFirstSample_;
    ready_ = -1; return true;
  }
  bool running() const { return running_; }
  int gpio() const { return gpio_; }
  esp_err_t error() const { return error_; }
  uint64_t samples() const { return samples_; }
  uint64_t frames() const { return frames_; }
  uint32_t droppedFrames() const { return droppedFrames_; }
  uint32_t poolOverflows() const { return overflows_; }
  uint32_t resyncs() const { return resyncs_; }
  uint32_t invalidResults() const { return invalidResults_; }
  uint64_t elapsedUs() const { return running_ ? esp_timer_get_time() - startUs_ : 0; }
  // Count/time is diagnostic throughput, not an independent sample-clock
  // calibration. It is misleading across a resync or dropped DMA results.

 private:
  adc_continuous_handle_t handle_ = nullptr;
  bool running_ = false;
  int gpio_ = -1, channel_ = -1;
  esp_err_t error_ = ESP_OK;
  alignas(4) uint8_t dma_[2048] = {};
  uint16_t raw_[2][FFT_N] = {};
  int filling_ = 0, ready_ = -1;
  size_t filled_ = 0;
  uint64_t samples_ = 0, frames_ = 0, readySequence_ = 0, readyFirstSample_ = 0;
  uint32_t droppedFrames_ = 0, resyncs_ = 0, invalidResults_ = 0;
  volatile uint32_t overflows_ = 0;
  uint32_t observedOverflows_ = 0;
  int64_t startUs_ = 0;
  static bool IRAM_ATTR overflowCallback(adc_continuous_handle_t,
      const adc_continuous_evt_data_t*, void* context) {
    ++static_cast<AudioCapture*>(context)->overflows_; return false;
  }
  void resetAssembly() { filling_ = 0; ready_ = -1; filled_ = 0; }
  bool failStart() {
    const esp_err_t original = error_; stop(); error_ = original; return false;
  }
  void resync() {
    // Never concatenate samples across a driver-pool overflow. Flush and start
    // a fresh frame; expose the interruption in counters for the operator.
    if (!handle_) return;
    esp_err_t e = adc_continuous_stop(handle_); running_ = false;
    resetAssembly(); ++resyncs_;
    if (e == ESP_OK) e = adc_continuous_flush_pool(handle_);
    if (e == ESP_OK) e = adc_continuous_start(handle_);
    if (e == ESP_OK) { running_ = true; error_ = ESP_OK; }
    else { error_ = e; failStart(); }
  }
};
} // namespace mc2live
