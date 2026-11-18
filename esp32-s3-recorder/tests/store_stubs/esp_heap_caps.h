#pragma once
#include <cstddef>
#include <cstdlib>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
inline bool mockPsramAvailable=true;
inline void* mockPsramAllocation=nullptr;
inline size_t mockPsramBytes=0;
inline void* heap_caps_malloc(size_t n,int caps){if(!mockPsramAvailable||caps!=(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT))return nullptr;mockPsramAllocation=std::malloc(n);mockPsramBytes=n;return mockPsramAllocation;}
inline void heap_caps_free(void* p){std::free(p);if(p==mockPsramAllocation){mockPsramAllocation=nullptr;mockPsramBytes=0;}}
