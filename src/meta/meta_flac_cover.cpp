#include "meta/meta_flac_cover.h"

static uint32_t read_u24_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}
static uint32_t read_u32_be(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static bool read_exact(File32& f, void* dst, size_t n) {
  return (size_t)f.read(dst, n) == n;
}
static bool skip_bytes(File32& f, uint32_t n) {
  while (n > 0) {
    uint32_t step = (n > 0x7FFFFFFF) ? 0x7FFFFFFF : n;
    if (!f.seekCur((int32_t)step)) return false;
    n -= step;
  }
  return true;
}

extern SemaphoreHandle_t g_sd_mutex;

bool flac_find_picture(SdFat& sd, const char* path, FlacCoverLoc& out)
{
  out = {};

  // 获取 SD 卡访问互斥锁
  if (xSemaphoreTake(g_sd_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
    return false;
  }

  File32 f = sd.open(path, O_RDONLY);
  if (!f) {
    xSemaphoreGive(g_sd_mutex);
    return false;
  }

  uint8_t magic[4];
  if (!read_exact(f, magic, 4)) { 
    f.close(); 
    xSemaphoreGive(g_sd_mutex);
    return false;
  }
  if (!(magic[0]=='f' && magic[1]=='L' && magic[2]=='a' && magic[3]=='C')) { 
    f.close(); 
    xSemaphoreGive(g_sd_mutex);
    return true;
  }

  // 逐块读取 metadata blocks
  while (true) {
    uint8_t bh[4];
    if (!read_exact(f, bh, 4)) break;

    bool is_last = (bh[0] & 0x80) != 0;
    uint8_t type = (bh[0] & 0x7F);
    uint32_t len = read_u24_be(bh + 1);

    if (type != 6 /*PICTURE*/) {
      if (!skip_bytes(f, len)) break;
      if (is_last) break;
      continue;
    }

    // ✅✅✅ 核心修复：此刻文件指针就是图片数据起点
    // 性能优化：一次性读取元数据到缓冲区，减少 SD 卡寻道
    static uint8_t meta_buffer[128]; // 足够容纳所有元数据字段
    size_t meta_read = 0;
    
    // 记录起始位置，用于计算最终的 data_off
    uint64_t base_pos = f.position();
    
    // 读取足够的元数据到缓冲区
    if (len > sizeof(meta_buffer)) {
      // 如果元数据太长，先读取基本字段
      if (!read_exact(f, meta_buffer, 32)) break;
      meta_read = 32;
    } else {
      // 如果元数据较短，一次性读取全部
      if (!read_exact(f, meta_buffer, len)) break;
      meta_read = len;
    }
    
    // 从缓冲区中解析数据
    size_t offset = 0;
    auto read_u32_from_buffer = [&](uint32_t& v)->bool {
      if (offset + 4 > meta_read) return false;
      v = ((uint32_t)meta_buffer[offset] << 24) | 
          ((uint32_t)meta_buffer[offset+1] << 16) | 
          ((uint32_t)meta_buffer[offset+2] << 8) | 
          (uint32_t)meta_buffer[offset+3];
      offset += 4;
      return true;
    };

    uint32_t picture_type=0, mime_len=0, desc_len=0;
    uint32_t w=0,h=0,depth=0,colors=0, data_len=0;

    if (!read_u32_from_buffer(picture_type)) break;
    
    // 只寻找 Front Cover (3) 类型的图片
    if (picture_type != 3) {
      if (!skip_bytes(f, len - meta_read)) break; // 跳过剩余的 PICTURE 块数据
      if (is_last) break;
      continue;
    }
    
    if (!read_u32_from_buffer(mime_len)) break;

    // 读 mime
    String mime;
    if (mime_len > 0 && mime_len < 128) {
      if (offset + mime_len <= meta_read) {
        // 从缓冲区中读取
        char tmp[129];
        memcpy(tmp, meta_buffer + offset, mime_len);
        tmp[mime_len] = 0;
        mime = String(tmp);
        offset += mime_len;
      } else {
        // 从文件中读取
        char tmp[129];
        if (f.read((uint8_t*)tmp, mime_len) != (int)mime_len) break;
        tmp[mime_len] = 0;
        mime = String(tmp);
        offset += mime_len; // 更新偏移量
      }
    } else if (mime_len > 0) {
      // 跳过过长 mime
      if (offset + mime_len <= meta_read) {
        offset += mime_len;
      } else {
        if (!f.seekCur((int32_t)(mime_len - (meta_read - offset)))) break;
        offset += mime_len; // 更新偏移量
      }
    }

    if (!read_u32_from_buffer(desc_len)) break;
    // 跳过 description
    if (desc_len > 0) {
      if (offset + desc_len <= meta_read) {
        offset += desc_len;
      } else {
        if (!f.seekCur((int32_t)(desc_len - (meta_read - offset)))) break;
        offset += desc_len; // 更新偏移量
      }
    }

    if (!read_u32_from_buffer(w)) break;
    if (!read_u32_from_buffer(h)) break;
    if (!read_u32_from_buffer(depth)) break;
    if (!read_u32_from_buffer(colors)) break;

    if (!read_u32_from_buffer(data_len)) break;

    // ✅✅✅ 核心修复：使用起始位置 + 解析偏移量计算 data_off，避免文件指针同步问题
    uint64_t data_off = base_pos + offset;

    // 输出
    out.found = (data_len > 0);
    out.offset = data_off;
    out.size = data_len;
    out.mime = mime;

    // （可选）把文件指针移动到 block 末尾
    if (data_len > 0) {
      if (!f.seekCur((int32_t)data_len)) {
        // 不影响我们已经拿到 offset/size
      }
    }

    f.close();
    // 释放 SD 卡访问互斥锁
    xSemaphoreGive(g_sd_mutex);
    return true;
  }

  f.close();
  // 释放 SD 卡访问互斥锁
  xSemaphoreGive(g_sd_mutex);
  return true;
}
