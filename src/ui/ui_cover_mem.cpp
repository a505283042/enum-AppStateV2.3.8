#include "ui/ui_cover_mem.h"
#include "esp32-hal-psram.h"
#include "utils/utils_log.h"

static bool file_exists(SdFat& sd, const char* path) {
  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;
  f.close();
  return true;
}

static bool detect_png_from_header(File32& f) {
  uint8_t b[8];
  uint32_t pos = f.curPosition();
  if (f.read(b, 8) != 8) { f.seekSet(pos); return false; }
  f.seekSet(pos);
  return (b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G' && b[4]==0x0D && b[5]==0x0A && b[6]==0x1A && b[7]==0x0A);
}

static bool detect_jpg_from_header(File32& f) {
  uint8_t b[2];
  uint32_t pos = f.curPosition();
  if (f.read(b, 2) != 2) { f.seekSet(pos); return false; }
  f.seekSet(pos);
  return (b[0]==0xFF && b[1]==0xD8);
}

static bool detect_png_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 8 && b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G' && b[4]==0x0D && b[5]==0x0A && b[6]==0x1A && b[7]==0x0A);
}

static bool detect_jpg_from_buffer(const uint8_t* b, size_t len) {
  return (len >= 2 && b[0]==0xFF && b[1]==0xD8);
}

// 400KB 上限：够大多数封面，避免炸内存
static constexpr size_t MAX_COVER_BYTES_HARD = 400 * 1024; // 防爆上限（可按你 PSRAM 情况调）
static uint8_t* s_cover_buf = nullptr;
static bool s_cover_initialized = false;

// 初始化封面缓冲区（在系统启动时调用一次）
bool cover_init_buffer(void)
{
  if (s_cover_initialized) return true;
  
  uint32_t ps_before   = ESP.getFreePsram();
  uint32_t heap_before = ESP.getFreeHeap();

  uint8_t* p = (uint8_t*)ps_malloc(MAX_COVER_BYTES_HARD);
  if (!p) p = (uint8_t*)malloc(MAX_COVER_BYTES_HARD);
  if (!p) return false;

  uint32_t ps_after   = ESP.getFreePsram();
  uint32_t heap_after = ESP.getFreeHeap();

  LOGI("COVER", "init=%u ptr=%p psram_used=%u heap_used=%u free_psram=%u free_heap=%u",
       (unsigned)MAX_COVER_BYTES_HARD, p,
       (unsigned)(ps_before - ps_after),
       (unsigned)(heap_before - heap_after),
       (unsigned)ps_after,
       (unsigned)heap_after);

  s_cover_buf = p;
  s_cover_initialized = true;
  return true;
}

static bool ensure_cover_buf(size_t need)
{
  if (need == 0 || need > MAX_COVER_BYTES_HARD) return false;
  
  // 检查缓冲区是否已初始化
  if (!s_cover_initialized) {
    LOGW("COVER", "buffer not initialized, call cover_init_buffer() first");
    return false;
  }
  
  // 缓冲区已分配固定大小，直接检查是否满足需求
  return (s_cover_buf != nullptr);
}

bool cover_ensure_buffer(size_t need)
{
  return ensure_cover_buf(need);
}

uint8_t* cover_get_buffer()
{
  return s_cover_buf;
}

bool cover_load_to_memory(SdFat& sd, const TrackInfo& t, const uint8_t*& out_ptr, size_t& out_len, bool& out_is_png)
{
  out_ptr = nullptr;
  out_len = 0;
  out_is_png = false;

  File32 f;

  // 1) 外部封面
  if (t.cover_source == COVER_FILE_FALLBACK && t.cover_path.length()) {
    f = sd.open(t.cover_path.c_str(), O_RDONLY);
    if (!f) return false;
    uint32_t sz = f.fileSize();
    if (sz == 0 || !ensure_cover_buf(sz)) { f.close(); return false; }
    int r = f.read(s_cover_buf, sz);
    f.close();
    if (r != (int)sz) return false;
    
    // 检测图片格式（PNG 或 JPG）
    out_is_png = detect_png_from_buffer(s_cover_buf, sz);
    
    out_ptr = s_cover_buf;
    out_len = sz;
    return true;
  }

  // 2) 内嵌封面
  if ((t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) &&
      t.cover_size > 0 && ensure_cover_buf(t.cover_size))
  {
    f = sd.open(t.audio_path.c_str(), O_RDONLY);
    if (!f) return false;
    if (!f.seekSet(t.cover_offset)) { f.close(); return false; }

    // 读头判断图片格式（PNG 或 JPG）
    out_is_png = detect_png_from_header(f);

    int r = f.read(s_cover_buf, t.cover_size);
    f.close();
    if (r != (int)t.cover_size) return false;

    out_ptr = s_cover_buf;
    out_len = t.cover_size;
    return true;
  }

  return false;
}
