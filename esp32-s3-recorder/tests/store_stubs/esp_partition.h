#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#define ESP_OK 0
#define ESP_PARTITION_TYPE_DATA 1
#define ESP_PARTITION_SUBTYPE_DATA_SPIFFS 2
struct esp_partition_t {size_t size=0x180000;};
inline esp_partition_t mockPartition;
inline std::vector<uint8_t> mockPartitionBytes(0x180000,0xFF);
inline const esp_partition_t* esp_partition_find_first(int,int,const char*){return &mockPartition;}
inline int esp_partition_read(const esp_partition_t*,size_t off,void* b,size_t n){if(off+n>mockPartitionBytes.size())return -1;std::memcpy(b,mockPartitionBytes.data()+off,n);return 0;}
