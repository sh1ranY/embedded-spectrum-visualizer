#pragma once
#include <cstdint>
struct MockWire { unsigned transactions=0; uint8_t result=0; bool begin(int,int,unsigned){return true;}void setTimeOut(unsigned){}void beginTransmission(uint8_t){}void write(uint8_t){}uint8_t endTransmission(){++transactions;return result;}};
extern MockWire Wire;
