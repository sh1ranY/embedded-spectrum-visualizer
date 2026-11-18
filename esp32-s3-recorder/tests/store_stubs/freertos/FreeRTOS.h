#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>
#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdMS_TO_TICKS(x) (x)
using BaseType_t = int;
using UBaseType_t = unsigned;
struct StaticQueue_t { size_t capacity = 0, itemSize = 0; std::deque<std::vector<uint8_t>> blocks; };
using QueueHandle_t = StaticQueue_t*;
using TaskHandle_t = void*;
