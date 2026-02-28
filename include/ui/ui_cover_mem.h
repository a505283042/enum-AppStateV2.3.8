#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "storage/storage_music.h"

// 把封面读到内存（buf 由内部静态缓冲提供）
// out_ptr 指向内部缓冲，下一次调用会覆盖
bool cover_load_to_memory(SdFat& sd, const TrackInfo& t, const uint8_t*& out_ptr, size_t& out_len, bool& out_is_png);

// 确保封面缓冲区足够大（内部使用）
bool cover_ensure_buffer(size_t need);

// 获取封面缓冲区指针（内部使用）
uint8_t* cover_get_buffer();
