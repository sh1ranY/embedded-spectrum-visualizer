#pragma once

// UNO R4 WiFi / Arduino Renesas 1.6.0: exclusive ADC0, A0 = P014 / AN009.
// Do not call analogRead(), analogReference(), DAC output or other ADC APIs
// while this sampler is active. No sound is stored beyond three RAM frames.
#include <Arduino.h>
#include <FspTimer.h>
#include <analog.h>
#include <cstring>

#if !defined(ARDUINO_UNOWIFIR4)
#error "This sampler requires the UNO R4 WiFi pin map"
#endif

namespace mc2r4 {

class Sampler {
 public:
  static constexpr uint16_t FRAME_SIZE = 512;
  static constexpr uint32_t REQUESTED_RATE = 20000;

  bool begin() {
    if (running_) return true;
    if (begun_) return false;  // Retry requires a normal board restart.
    begun_ = true;
    std::memset(&adc_.ctrl, 0, sizeof(adc_.ctrl));
    adc_.cfg.resolution = ADC_RESOLUTION_12_BIT;
    adc_.cfg.mode = ADC_MODE_SINGLE_SCAN;
    adc_.cfg.trigger = ADC_TRIGGER_SOFTWARE;
    adc_.cfg_extend.clearing = ADC_CLEAR_AFTER_READ_OFF;
    adc_.channel_cfg.scan_mask = uint32_t(1) << 9;
    adc_.channel_cfg.scan_mask_group_b = 0;
    adc_.channel_cfg.add_mask = 0;
    pinPeripheral(BSP_IO_PORT_00_PIN_14, IOPORT_CFG_ANALOG_ENABLE);
    fsp_err_t result = R_ADC_Open(&adc_.ctrl, &adc_.cfg);
    if (result != FSP_SUCCESS) return fail(1, result);
    adcOpen_ = true;
    result = R_ADC_ScanCfg(&adc_.ctrl, &adc_.channel_cfg);
    if (result != FSP_SUCCESS) return fail(2, result);

    uint8_t timerType = GPT_TIMER;
    const int8_t timerIndex = FspTimer::get_available_timer(timerType);
    if (timerIndex < 0) return fail(3, FSP_ERR_NOT_OPEN);
    if (!timer_.begin(TIMER_MODE_PERIODIC, timerType, uint8_t(timerIndex),
                      float(REQUESTED_RATE), 0.0f, timerCallback, this))
      return fail(4, FSP_ERR_NOT_OPEN);
    // Higher priority than USB/Wire: keep audio sample starts regularly spaced.
    // This ISR never waits for conversion or invokes display/serial code.
    if (!timer_.setup_overflow_irq(5) || !timer_.open())
      return fail(5, FSP_ERR_NOT_OPEN);
    timerOpen_ = true;
    const uint32_t period = timer_.get_period_raw();
    if (!period) return fail(6, FSP_ERR_INVALID_ARGUMENT);
    rate_ = timer_.get_freq_hz() / period;
    if (rate_ < 19000 || rate_ > 21000)
      return fail(7, FSP_ERR_INVALID_ARGUMENT);

    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    cyclesPerUs_ = SystemCoreClock / 1000000UL;
    expectedCycles_ = SystemCoreClock / rate_;
    result = R_ADC_ScanStart(&adc_.ctrl);
    if (result != FSP_SUCCESS) return fail(8, result);
    // First timer tick reads this initial conversion. Subsequent samples were
    // started by the previous tick; no conversion-time polling in the ISR.
    running_ = true;
    if (!timer_.start()) return fail(9, FSP_ERR_NOT_OPEN);
    return true;
  }

  // Latest complete frame; copy while IRQs run, using a separately held buffer.
  // Skipped older display frames are counted separately from ADC timing faults.
  bool takeFrame(uint16_t out[FRAME_SIZE]) {
    if (!out || fault_) return false;
    const uint32_t irqState = __get_PRIMASK();
    __disable_irq();
    const int8_t index = ready_;
    if (index >= 0) { reading_ = index; ready_ = -1; }
    __DMB();
    __set_PRIMASK(irqState);
    if (index < 0) return false;
    for (uint16_t n = 0; n < FRAME_SIZE; ++n) out[n] = buffers_[index][n];
    const uint32_t doneState = __get_PRIMASK();
    __disable_irq();
    __DMB();
    reading_ = -1;
    __set_PRIMASK(doneState);
    return true;
  }

  uint32_t sampleRate() const { return rate_; }
  uint32_t totalSamples() const { return samples_; }
  uint32_t overruns() const { return droppedFrames_; }
  uint32_t adcBusyCount() const { return busy_; }
  uint32_t lateTicks() const { return late_; }
  uint32_t minIntervalUs() const {
    return minCycles_ == UINT32_MAX || !cyclesPerUs_ ? 0 : minCycles_ / cyclesPerUs_;
  }
  uint32_t maxIntervalUs() const { return cyclesPerUs_ ? maxCycles_ / cyclesPerUs_ : 0; }
  bool fault() const { return fault_; }
  uint8_t errorCode() const { return error_; }
  uint32_t lastFspError() const { return fspError_; }

 private:
  ADC_Container adc_{0, nullptr};
  FspTimer timer_;
  bool begun_ = false, adcOpen_ = false, timerOpen_ = false;
  volatile bool running_ = false, fault_ = false;
  volatile uint8_t error_ = 0;
  volatile uint32_t fspError_ = 0;
  uint32_t rate_ = 0, cyclesPerUs_ = 0, expectedCycles_ = 0;
  volatile uint16_t buffers_[3][FRAME_SIZE] = {};
  volatile uint16_t position_ = 0;
  volatile int8_t writing_ = 0, ready_ = -1, reading_ = -1;
  volatile uint32_t samples_ = 0, droppedFrames_ = 0, busy_ = 0, late_ = 0;
  volatile uint32_t minCycles_ = UINT32_MAX, maxCycles_ = 0, previousCycle_ = 0;
  volatile uint8_t consecutiveBusy_ = 0;

  bool fail(uint8_t code, fsp_err_t result) {
    running_ = false; fault_ = true; error_ = code; fspError_ = uint32_t(result);
    if (timerOpen_) { timer_.stop(); timer_.close(); timerOpen_ = false; }
    if (adcOpen_) { R_ADC_ScanStop(&adc_.ctrl); R_ADC_Close(&adc_.ctrl); adcOpen_ = false; }
    return false;
  }

  static void timerCallback(timer_callback_args_t* args) {
    if (!args || args->event != TIMER_EVENT_CYCLE_END || !args->p_context) return;
    const_cast<Sampler*>(static_cast<const Sampler*>(args->p_context))->sampleTick();
  }

  void sampleTick() {
    if (!running_) return;
    const uint32_t now = DWT->CYCCNT;
    if (previousCycle_) {
      const uint32_t interval = now - previousCycle_;
      if (interval < minCycles_) minCycles_ = interval;
      if (interval > maxCycles_) maxCycles_ = interval;
      if (interval > expectedCycles_ + expectedCycles_ / 2) ++late_;
    }
    previousCycle_ = now;

    // A conversion that overruns its 50 us budget is visible, never silently
    // duplicated as an apparently valid equally spaced audio sample.
    if (adc_.ctrl.p_reg->ADCSR_b.ADST) {
      ++busy_;
      position_ = 0;  // discard discontinuous partial frame
      if (++consecutiveBusy_ >= 4) {
        running_ = false; fault_ = true; error_ = 10;
      }
      return;
    }
    consecutiveBusy_ = 0;
    const uint16_t value = adc_.ctrl.p_reg->ADDR[9] & 0x0FFFu;
    // Start the next conversion before buffer bookkeeping. ADC0 has exactly
    // AN009 enabled, software-triggered single scan, 12-bit right-aligned.
    adc_.ctrl.p_reg->ADCSR_b.ADST = 1;
    ++samples_;
    buffers_[writing_][position_++] = value;
    if (position_ != FRAME_SIZE) return;
    position_ = 0;
    if (ready_ >= 0) ++droppedFrames_;
    const int8_t completed = writing_;
    for (int8_t n = 0; n < 3; ++n) {
      if (n != completed && n != reading_) { writing_ = n; break; }
    }
    __DMB();
    ready_ = completed;
  }
};

}  // namespace mc2r4
