#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#include <SdFat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

struct AudioFile {
  File32 f;
  uint32_t _cached_size;  // 缓存文件大小，避免频繁查询
  
  bool open(SdFat& sd, const char* path);
  void close();
  ssize_t read(void* dst, size_t bytes);   // 返回实际读到字节数（使用 ssize_t 避免溢出）
  bool seek(uint32_t pos);
  uint32_t tell();
  uint32_t size();
};

// SD 卡访问互斥锁（全局，防止多线程冲突）
extern SemaphoreHandle_t g_sd_mutex;
