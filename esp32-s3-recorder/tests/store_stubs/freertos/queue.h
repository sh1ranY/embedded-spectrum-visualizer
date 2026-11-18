#pragma once
#include "FreeRTOS.h"
#include <cstring>
inline QueueHandle_t xQueueCreateStatic(size_t n,size_t size,uint8_t*,StaticQueue_t* q){q->capacity=n;q->itemSize=size;q->blocks.clear();return q;}
inline int xQueueReset(QueueHandle_t q){q->blocks.clear();return pdTRUE;}
inline int xQueueSend(QueueHandle_t q,const void* b,int){if(q->blocks.size()>=q->capacity)return pdFALSE;q->blocks.emplace_back(q->itemSize);std::memcpy(q->blocks.back().data(),b,q->itemSize);return pdTRUE;}
inline int xQueueReceive(QueueHandle_t q,void* b,int){if(q->blocks.empty())return pdFALSE;std::memcpy(b,q->blocks.front().data(),q->itemSize);q->blocks.pop_front();return pdTRUE;}
inline unsigned uxQueueMessagesWaiting(QueueHandle_t q){return unsigned(q->blocks.size());}
