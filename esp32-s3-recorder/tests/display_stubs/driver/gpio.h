#pragma once
#include <cstdint>
#include <vector>
using gpio_num_t=int;
using esp_err_t=int;
constexpr int ESP_OK=0,GPIO_NUM_19=19,GPIO_NUM_20=20;
constexpr int GPIO_MODE_INPUT=1,GPIO_MODE_OUTPUT=2,GPIO_PULLUP_ENABLE=1,GPIO_PULLUP_DISABLE=0,GPIO_PULLDOWN_DISABLE=0,GPIO_INTR_DISABLE=0;
struct gpio_config_t { uint64_t pin_bit_mask=0; int mode=0,pull_up_en=0,pull_down_en=0,intr_type=0; };
extern int testLevels[64];
extern std::vector<std::vector<uint8_t>> shifts;
extern std::vector<uint8_t> currentShift;
extern uint8_t bitByte;extern int bitCount;
inline esp_err_t gpio_pullup_dis(int){return ESP_OK;}
inline esp_err_t gpio_config(const gpio_config_t*){return ESP_OK;}
inline int gpio_get_level(int pin){return testLevels[pin];}
inline esp_err_t gpio_set_level(int pin,int value){
 int old=testLevels[pin]; testLevels[pin]=value;
 if(pin==14 && value==0){currentShift.clear();bitByte=0;bitCount=0;}
 if(pin==47 && old==0 && value==1 && testLevels[14]==0){bitByte=(bitByte<<1)|testLevels[21];if(++bitCount==8){currentShift.push_back(bitByte);bitCount=0;bitByte=0;}}
 if(pin==14 && value==1 && !currentShift.empty()){shifts.push_back(currentShift);currentShift.clear();}
 return ESP_OK;
}
