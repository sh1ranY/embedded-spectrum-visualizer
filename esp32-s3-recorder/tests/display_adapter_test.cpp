#include <cassert>
#include <iostream>
#include "Arduino.h"
#include "Wire.h"
#include "driver/gpio.h"
uint32_t testNow=0;int testLevels[64]={};MockWire Wire;
std::vector<std::vector<uint8_t>> shifts;std::vector<uint8_t>currentShift;uint8_t bitByte=0;int bitCount=0;
#include "../firmware/MC2_Live/live_displays.h"
static void tick(mc2hw::LiveDisplays& h,uint32_t t){testNow=t;h.tick(t);}
int main(){
 testLevels[19]=testLevels[20]=1;
 mc2hw::LiveDisplays h;assert(h.begin(0));assert(!h.matrixAwake());
 for(auto &s:shifts)assert(!(s.size()==8 && s[0]==15 && s[1]!=0));
 assert(!h.buttonsArmed());tick(h,0);tick(h,59);assert(!h.buttonsArmed());tick(h,60);assert(h.buttonsArmed());
 testLevels[20]=0;tick(h,70);tick(h,99);assert(!h.takeButtons().redPressed);tick(h,100);assert(h.takeButtons().redPressed);assert(!h.takeButtons().redPressed);
 testLevels[20]=1;tick(h,110);tick(h,139);assert(!h.takeButtons().redReleased);tick(h,140);assert(h.takeButtons().redReleased);assert(h.redPresses()==1);
 uint8_t pixels[8][4]={};pixels[0][0]=0x80;h.showRows(pixels);auto &f=shifts[shifts.size()-8];assert(f.size()==8);assert(f[0]==1&&f[1]==0x80);for(unsigned i=1;i<4;i++)assert(f[2*i]==1&&f[2*i+1]==0);
 h.wakeMatrix();assert(h.matrixAwake());h.wakeMatrix(false);assert(!h.matrixAwake());
 h.setText("DEMO TIME","750 Hz");for(uint32_t t=145;t<600;t++)tick(h,t);assert(h.lcdReady());
 unsigned settled=Wire.transactions;for(uint32_t t=600;t<1100;t++)tick(h,t);assert(Wire.transactions==settled);
 Wire.result=4;h.setText("DEMO FREQ","3000 Hz");for(uint32_t t=1100;t<1500;t++)tick(h,t);assert(h.lcdFault());unsigned faultTx=Wire.transactions;for(uint32_t t=1500;t<2500;t++)tick(h,t);assert(Wire.transactions==faultTx);
 Wire.result=0;assert(h.retryLcd(2500));for(uint32_t t=2500;t<3000;t++)tick(h,t);assert(h.lcdReady());assert(h.i2cErrors()==1);
 // A button held through boot must not generate a synthetic press/release.
 testLevels[19]=0;testLevels[20]=1;mc2hw::LiveDisplays held;testNow=3000;assert(held.begin(3000));tick(held,3100);assert(!held.buttonsArmed());testLevels[19]=1;tick(held,3110);tick(held,3140);assert(!held.takeButtons().greenReleased);tick(held,3200);assert(held.buttonsArmed());assert(!held.takeButtons().greenReleased);
 // Unsigned elapsed arithmetic arms correctly across the millis wrap.
 testNow=0xFFFFFFE0u;mc2hw::LiveDisplays wrap;assert(wrap.begin(testNow));tick(wrap,0xFFFFFFE0u);tick(wrap,0x0000001Bu);assert(!wrap.buttonsArmed());tick(wrap,0x0000001Cu);assert(wrap.buttonsArmed());
 std::cout<<"PASS matrix chip mapping, test-mode off/startup shutdown, debounce/boot/wrap boundaries, LCD slow init/idle/no-retry fault/explicit recovery"<<std::endl;
}
