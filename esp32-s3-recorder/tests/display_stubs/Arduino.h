#pragma once
#include <cstdint>
#define CONFIG_IDF_TARGET_ESP32S3 1
#define ARDUINO_USB_MODE 0
#define ARDUINO_USB_CDC_ON_BOOT 0
#define ARDUINO_USB_MSC_ON_BOOT 0
#define ARDUINO_USB_DFU_ON_BOOT 0
extern uint32_t testNow;
inline uint32_t millis(){return testNow;}
inline void delayMicroseconds(unsigned){}
