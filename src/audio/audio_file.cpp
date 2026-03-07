#include "audio/audio_file.h"
#include "utils/log.h"

// SD 卡访问互斥锁（全局，防止多线程冲突）
SemaphoreHandle_t g_sd_mutex = nullptr;

bool AudioFile::open(SdFat& sd, const char* path) {
  f = sd.open(path, O_RDONLY);
  if (!f) return false;
  
  // 缓存文件大小，避免频繁查询
  _cached_size = f.fileSize();
  
  return true;
}

void AudioFile::close() { 
  if (f) f.close(); 
}

ssize_t AudioFile::read(void* dst, size_t bytes) {
  if (!f) return -1;
  
  // 获取 SD 卡互斥锁，防止多线程冲突（切歌时可能需要等待 UI 线程加载封面完成）
  if (g_sd_mutex == nullptr) {
    LOGE("[AudioFile] SD 互斥锁未初始化");
    return -1;
  }
  
  if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    LOGE("[AudioFile] 获取 SD 锁超时");
    return -1;
  }
  
  // 所有涉及 f 成员的操作都在锁内
  uint32_t current_pos = f.curPosition();
  
  if (current_pos >= _cached_size) {
    // 已到达文件末尾，返回 0 表示 EOF
    xSemaphoreGive(g_sd_mutex);
    return 0;
  }
  
  // 计算实际可读取的字节数
  uint32_t remaining = _cached_size - current_pos;
  if (remaining > bytes) {
    remaining = bytes;
  }
  
  // 执行读取
  int n = f.read(dst, remaining);
  
  // 释放 SD 卡互斥锁
  xSemaphoreGive(g_sd_mutex);
  
  // 处理读取结果
  if (n < 0) {
    // 读取错误
    return -1;
  } else if (n == 0 && remaining > 0) {
    // 应该能读取但返回 0，可能是 SD 卡问题
    LOGE("[AudioFile] 读取异常：期望 %u 字节但返回 0", remaining);
    return -1;
  }
  
  return n;
}

bool AudioFile::seek(uint32_t pos) { 
  if (!f) {
    LOGE("[AudioFile] Seek 失败：文件未打开");
    return false;
  }
  
  // 获取 SD 卡互斥锁（切歌时可能需要等待 UI 线程加载封面完成）
  if (g_sd_mutex == nullptr) {
    LOGE("[AudioFile] SD 互斥锁未初始化");
    return false;
  }
  
  if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
    LOGE("[AudioFile] 获取 SD 锁超时");
    return false;
  }
  
  // 边界检查：使用缓存的文件大小
  if (pos > _cached_size) {
    LOGW("[AudioFile] Seek 超出范围：请求 %u，文件大小 %u", pos, _cached_size);
    pos = _cached_size;
  }
  
  bool result = f.seekSet(pos);
  
  // 释放 SD 卡互斥锁
  xSemaphoreGive(g_sd_mutex);
  
  if (!result) {
    LOGE("[AudioFile] Seek 失败：位置 %u，文件大小 %u", pos, _cached_size);
  }
  
  return result;
}

uint32_t AudioFile::tell() { 
  return f ? (uint32_t)f.curPosition() : 0; 
}

uint32_t AudioFile::size() { 
  return f ? _cached_size : 0; 
}
