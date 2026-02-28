#include "ui/ui_cover.h"

static bool file_exists(SdFat& sd, const char* path) {
  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;
  f.close();
  return true;
}

static bool copy_bytes(File32& in, File32& out, uint32_t n) {
  uint8_t buf[512];
  while (n > 0) {
    uint32_t chunk = (n > sizeof(buf)) ? sizeof(buf) : n;
    int r = in.read(buf, chunk);
    if (r <= 0) return false;
    if (out.write(buf, (size_t)r) != (size_t)r) return false;
    n -= (uint32_t)r;
  }
  return true;
}

static bool detect_is_png(SdFat& sd, const char* path) {
  File32 f = sd.open(path, O_RDONLY);
  if (!f) return false;
  uint8_t b[8];
  bool ok = (f.read(b, 8) == 8 &&
             b[0]==0x89 && b[1]=='P' && b[2]=='N' && b[3]=='G' &&
             b[4]==0x0D && b[5]==0x0A && b[6]==0x1A && b[7]==0x0A);
  f.close();
  return ok;
}

bool cover_export_to_temp(SdFat& sd, const TrackInfo& t, const char* temp_path)
{
  // 1) 外部 fallback：直接用 cover_path，不需要导出
  if (t.cover_source == COVER_FILE_FALLBACK && t.cover_path.length()) {
    // 但为了统一接口，你也可以选择复制一份到 temp（非必须）
    return file_exists(sd, t.cover_path.c_str());
  }

  // 2) 内嵌封面：从 audio_path 的 offset/size 截取到 temp_path
  if ((t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) &&
      t.cover_size > 0)
  {
    File32 in = sd.open(t.audio_path.c_str(), O_RDONLY);
    if (!in) return false;
    if (!in.seekSet(t.cover_offset)) { in.close(); return false; }

    File32 out = sd.open(temp_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (!out) { in.close(); return false; }

    bool ok = copy_bytes(in, out, t.cover_size);

    out.close();
    in.close();
    return ok;
  }

  return false;
}

bool ui_draw_cover(LGFX& tft, SdFat& sd, const TrackInfo& t, int x, int y, int w, int h)
{
  // 统一临时文件路径（隐藏文件避免污染）
  const char* tmp = "/Music/.cover_tmp";

  // 如果是外部封面，直接画 cover_path
  if (t.cover_source == COVER_FILE_FALLBACK && t.cover_path.length()) {
    // 判断 png/jpg
    if (detect_is_png(sd, t.cover_path.c_str())) {
      return tft.drawPngFile(sd, t.cover_path.c_str(), x, y, w, h);
    } else {
      return tft.drawJpgFile(sd, t.cover_path.c_str(), x, y, w, h);
    }
  }

  // 内嵌封面：导出到临时文件（扩展名对某些 LGFX 接口重要）
  // 用 mime 决定扩展名；没有 mime 就先当 jpg（多数封面是 jpg）
  String tmpPath = String(tmp) + ((t.cover_mime == "image/png") ? ".png" : ".jpg");

  if (!cover_export_to_temp(sd, t, tmpPath.c_str())) return false;

  if (tmpPath.endsWith(".png")) {
    return tft.drawPngFile(sd, tmpPath.c_str(), x, y, w, h);
  } else {
    return tft.drawJpgFile(sd, tmpPath.c_str(), x, y, w, h);
  }
}
