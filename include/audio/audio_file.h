#pragma once
#include <stdint.h>
#include <stddef.h>
#include <SdFat.h>

struct AudioFile {
  File32 f;
  bool open(SdFat& sd, const char* path);
  void close();
  int  read(void* dst, size_t bytes);   // 返回实际读到字节数
  bool seek(uint32_t pos);
  uint32_t tell();
  uint32_t size();
};
