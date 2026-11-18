#pragma once

// UNO R4 WiFi physical display/button adapter. The LCD timing and logical
// matrix mapping are retained from the previously visually verified ESP32
// adapter; actual R4 wiring and visual output still require a physical check.
// This header does not access ADC, storage, the camera, or the serial port.
#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <cstring>

#if !defined(ARDUINO_ARCH_RENESAS_UNO)
#error "This physical pin map requires the Arduino UNO R4 Renesas core"
#endif

namespace mc2hw {

struct ButtonEvents {
  bool greenPressed = false;
  bool greenReleased = false;
  bool redPressed = false;
  bool redReleased = false;
};

class LiveDisplays {
 public:
  static constexpr uint8_t GREEN = 3, RED = 4;
  static constexpr uint8_t DIN = 11, CS = 10, CLK = 13;
  static constexpr uint8_t SDA_PIN = A4, SCL_PIN = A5, LCD_ADDRESS = 0x27;
  static constexpr uint32_t DEBOUNCE_MS = 30, BOOT_RELEASE_MS = 60;

  // Initialises the specified digital connections. MAX is cleared and
  // held in shutdown; wakeMatrix() is a separate deliberate action by the app.
  // LCD uses the v1.2 conservative start-up and 5ms between transmitted bytes.
  bool begin(uint32_t now = millis()) {
    pinMode(GREEN, INPUT_PULLUP);
    pinMode(RED, INPUT_PULLUP);
    for (uint8_t n = 0; n < 2; ++n) {
      buttons_[n].raw = buttons_[n].stable =
          digitalRead(n ? RED : GREEN);
      buttons_[n].changedAt = now;
    }
    armed_ = false;
    releaseTiming_ = false;
    events_ = {};

    // Default SPI maps MOSI=D11 and SCK=D13 on UNO R4 WiFi. CS is GPIO D10.
    digitalWrite(CS, HIGH);
    pinMode(CS, OUTPUT);
    SPI.begin();
    digitalWrite(CS, HIGH);
    matrixConfigured_ = true;
    matrixAwake_ = false;
    maxAll(0x0F, 0);  // Display-test otherwise overrides shutdown/brightness.
    maxAll(0x0C, 0);
    maxAll(0x09, 0);
    maxAll(0x0A, 0);  // Minimum intensity; never use all-pixel test mode.
    maxAll(0x0B, 7);
    clearMatrix();

    setText("ELEC281 MC2", "STARTING");
    retryLcd(now);
    return true;
  }

  void tick(uint32_t now = millis()) {
    buttonsTick(now);
    lcdTick(now);
  }

  ButtonEvents takeButtons() {
    const ButtonEvents result = events_;
    events_ = {};
    return result;
  }

  bool buttonsArmed() const { return armed_; }
  int greenLevel() const { return buttons_[0].stable; }
  int redLevel() const { return buttons_[1].stable; }
  uint32_t greenPresses() const { return buttons_[0].presses; }
  uint32_t redPresses() const { return buttons_[1].presses; }

  void wakeMatrix(bool wake = true) {
    if (!matrixConfigured_) return;
    maxAll(0x0F, 0);
    maxAll(0x0C, wake ? 1 : 0);
    matrixAwake_ = wake;
  }
  bool matrixAwake() const { return matrixAwake_; }

  // Logical image: rows[top..bottom][left module..right module], bit7=left.
  // The physical v1.2 test established upright digits4,3,2,1 left-to-right,
  // hence logical modules0..3 map to serial chip indices3..0.
  void showRows(const uint8_t logicalRows[8][4]) {
    if (!matrixConfigured_ || !logicalRows) return;
    for (uint8_t row = 0; row < 8; ++row)
      for (uint8_t logical = 0; logical < 4; ++logical)
        matrixRows_[3 - logical][row] = logicalRows[row][logical];
    maxFrame();
  }

  void clearMatrix() {
    std::memset(matrixRows_, 0, sizeof(matrixRows_));
    if (matrixConfigured_) maxFrame();
  }

  // A frame in flight is a stable snapshot; mode updates cannot mix labels
  // half-way through its two rows. New text is picked up after that frame.
  void setText(const char* first, const char* second) {
    padLine(desired_[0], first);
    padLine(desired_[1], second);
  }

  bool lcdReady() const { return lcdActive_ && !lcdFault_; }
  bool lcdFault() const { return lcdFault_; }
  uint8_t lcdLastError() const { return lastI2cError_; }
  uint32_t i2cTransactions() const { return i2cTransactions_; }
  uint32_t i2cErrors() const { return i2cErrors_; }

  // Explicit retry only. An I2C failure stops further writes, avoiding a busy
  // retry loop that could stall sampling or generate misleading LCD status.
  bool retryLcd(uint32_t now = millis()) {
    lcdActive_ = false;
    lcdFault_ = false;
    initStep_ = writeStep_ = -1;
    lastI2cError_ = 0;
    if (!i2cStarted_) {
      // Renesas Wire.begin() returns void; its default bus is A4/A5.
      Wire.begin();
      Wire.setClock(100000);
      Wire.setWireTimeout(20000);  // Renesas API takes microseconds.
      i2cStarted_ = true;
    }
    if (!i2cStarted_) { failLcd(0xFF); return false; }
    Wire.beginTransmission(LCD_ADDRESS);
    const uint8_t result = Wire.endTransmission();
    ++i2cTransactions_;
    if (result) { failLcd(result); return false; }
    initStep_ = 0;
    lcdNext_ = now + 150;
    shownValid_ = false;
    return true;
  }

 private:
  struct Button {
    int raw = 1, stable = 1;
    uint32_t changedAt = 0, presses = 0;
  } buttons_[2];
  ButtonEvents events_;
  bool armed_ = false, releaseTiming_ = false;
  uint32_t releasedAt_ = 0;
  bool matrixConfigured_ = false, matrixAwake_ = false;
  uint8_t matrixRows_[4][8] = {};
  bool i2cStarted_ = false, lcdActive_ = false, lcdFault_ = false;
  bool shownValid_ = false;
  uint8_t lastI2cError_ = 0;
  uint32_t i2cTransactions_ = 0, i2cErrors_ = 0;
  int initStep_ = -1, writeStep_ = -1;
  uint32_t lcdNext_ = 0, lcdRefreshAt_ = 0;
  char desired_[2][17] = {}, writing_[2][17] = {}, shown_[2][17] = {};

  static bool due(uint32_t now, uint32_t deadline) {
    return int32_t(now - deadline) >= 0;
  }

  static void padLine(char (&dest)[17], const char* source) {
    std::memset(dest, ' ', 16);
    dest[16] = 0;
    if (!source) return;
    for (uint8_t n = 0; n < 16 && source[n]; ++n) dest[n] = source[n];
  }

  void buttonsTick(uint32_t now) {
    for (uint8_t n = 0; n < 2; ++n) {
      Button& b = buttons_[n];
      const int raw = digitalRead(n ? RED : GREEN);
      if (raw != b.raw) { b.raw = raw; b.changedAt = now; }
      if (b.stable != b.raw && uint32_t(now - b.changedAt) >= DEBOUNCE_MS) {
        b.stable = b.raw;
        if (armed_) {
          if (!b.stable) ++b.presses;
          if (n == 0) {
            if (!b.stable) events_.greenPressed = true;
            else events_.greenReleased = true;
          } else {
            if (!b.stable) events_.redPressed = true;
            else events_.redReleased = true;
          }
        }
      }
    }
    if (!armed_) {
      if (buttons_[0].stable == 1 && buttons_[1].stable == 1) {
        if (!releaseTiming_) { releaseTiming_ = true; releasedAt_ = now; }
        if (uint32_t(now - releasedAt_) >= BOOT_RELEASE_MS) armed_ = true;
      } else releaseTiming_ = false;
    }
  }

  void maxAll(uint8_t reg, uint8_t data) {
    uint8_t frame[8];
    for (uint8_t n = 0; n < 4; ++n) {
      frame[2 * n] = reg;
      frame[2 * n + 1] = data;
    }
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    digitalWrite(CS, LOW);
    SPI.transfer(frame, sizeof(frame));
    digitalWrite(CS, HIGH);
    SPI.endTransaction();
  }
  void maxFrame() {
    SPI.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE0));
    for (uint8_t row = 0; row < 8; ++row) {
      uint8_t frame[8];
      uint8_t n = 0;
      for (int chip = 3; chip >= 0; --chip) {
        frame[n++] = row + 1;
        frame[n++] = matrixRows_[chip][row];
      }
      digitalWrite(CS, LOW);
      SPI.transfer(frame, sizeof(frame));
      digitalWrite(CS, HIGH);
    }
    SPI.endTransaction();
  }

  void failLcd(uint8_t error) {
    ++i2cErrors_;
    lastI2cError_ = error;
    lcdFault_ = true;
    lcdActive_ = false;
    initStep_ = writeStep_ = -1;
  }
  bool pcfWrite(uint8_t data) {
    if (lcdFault_) return false;
    Wire.beginTransmission(LCD_ADDRESS);
    Wire.write(data);
    const uint8_t result = Wire.endTransmission();
    ++i2cTransactions_;
    if (result) { failLcd(result); return false; }
    return true;
  }
  bool lcdNibble(uint8_t nibble, bool rs = false) {
    const uint8_t data = (nibble & 0xF0) | (rs ? 1 : 0) | 8;
    if (!pcfWrite(data) || !pcfWrite(data | 4)) return false;
    delayMicroseconds(1);
    if (!pcfWrite(data)) return false;
    delayMicroseconds(40);
    return true;
  }
  bool lcdByte(uint8_t value, bool rs = false) {
    return lcdNibble(value & 0xF0, rs) && lcdNibble(uint8_t(value << 4), rs);
  }
  void queueFrame(uint32_t now) {
    std::memcpy(writing_, desired_, sizeof(writing_));
    writeStep_ = 0;
    lcdNext_ = now;
  }
  void lcdTick(uint32_t now) {
    if (lcdFault_ || !i2cStarted_) return;
    if (initStep_ >= 0) {
      if (!due(now, lcdNext_)) return;
      bool ok = true;
      uint32_t waitMs = 5;
      switch (initStep_) {
        case 0: case 1: case 2: ok = lcdNibble(0x30); break;
        case 3: ok = lcdNibble(0x20); break;
        case 4: ok = lcdByte(0x28); break;
        case 5: ok = lcdByte(0x0C); break;
        case 6: ok = lcdByte(0x06); break;
        case 7: ok = lcdByte(0x01); waitMs = 10; break;
        default:
          initStep_ = -1;
          lcdActive_ = true;
          queueFrame(millis());
          return;
      }
      if (ok) { ++initStep_; lcdNext_ = millis() + waitMs; }
      return;
    }
    if (!lcdActive_) return;
    if (writeStep_ >= 0 && due(now, lcdNext_)) {
      bool ok;
      if (writeStep_ == 0) ok = lcdByte(0x80);
      else if (writeStep_ <= 16) ok = lcdByte(writing_[0][writeStep_ - 1], true);
      else if (writeStep_ == 17) ok = lcdByte(0xC0);
      else ok = lcdByte(writing_[1][writeStep_ - 18], true);
      if (ok && ++writeStep_ >= 34) {
        std::memcpy(shown_, writing_, sizeof(shown_));
        shownValid_ = true;
        writeStep_ = -1;
        lcdRefreshAt_ = millis() + 250;
      }
      lcdNext_ = millis() + 5;
    } else if (writeStep_ < 0 && due(now, lcdRefreshAt_) &&
               (!shownValid_ || std::memcmp(shown_, desired_, sizeof(shown_)) != 0)) {
      queueFrame(now);
    }
  }
};

}  // namespace mc2hw
