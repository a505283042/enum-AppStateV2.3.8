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

bool flac_find_picture(SdFat& sd, const char* path, FlacCoverLoc& out)
{
  out = {};

  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;

  uint8_t magic[4];
  if (!read_exact(f, magic, 4)) { f.close(); return false; }
  if (!(magic[0]=='f' && magic[1]=='L' && magic[2]=='a' && magic[3]=='C')) { f.close(); return true; }

  // 逐块读取 metadata blocks
  for (int i = 0; i < 64; i++) {
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
    uint8_t b4[4];
    auto read_u32_be = [&](uint32_t& v)->bool {
      if (f.read(b4, 4) != 4) return false;
      v = ((uint32_t)b4[0] << 24) | ((uint32_t)b4[1] << 16) | ((uint32_t)b4[2] << 8) | (uint32_t)b4[3];
      return true;
    };

    uint32_t picture_type=0, mime_len=0, desc_len=0;
    uint32_t w=0,h=0,depth=0,colors=0, data_len=0;

    if (!read_u32_be(picture_type)) break;
    if (!read_u32_be(mime_len)) break;

    // 读 mime
    String mime;
    if (mime_len > 0 && mime_len < 128) {
      char tmp[129];
      if (f.read((uint8_t*)tmp, mime_len) != (int)mime_len) break;
      tmp[mime_len] = 0;
      mime = String(tmp);
    } else {
      // 跳过过长 mime
      if (!f.seekCur((int32_t)mime_len)) break;
    }

    if (!read_u32_be(desc_len)) break;
    // 跳过 description
    if (desc_len > 0) {
      if (!f.seekCur((int32_t)desc_len)) break;
    }

    if (!read_u32_be(w)) break;
    if (!read_u32_be(h)) break;
    if (!read_u32_be(depth)) break;
    if (!read_u32_be(colors)) break;

    if (!read_u32_be(data_len)) break;

    // ✅✅✅ 核心修复：此刻文件指针就是图片数据起点
    uint32_t data_off = (uint32_t)f.position();

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
    return true;
  }

  f.close();
  return true;
}
