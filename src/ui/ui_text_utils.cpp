#include "ui/ui_text_utils.h"
#include "ui/gc9a01_lgfx.h"
#include <math.h>

// 封面尺寸常量（需要与 ui.cpp 保持一致）
static constexpr int COVER_SIZE = 240;

// 外部声明 TFT 对象
extern LGFX tft;

// UTF-8 字符长度计算
static int utf8_char_len(uint8_t c)
{
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

void draw_center_text(const char* s, int y)
{
  extern lgfx::U8g2font g_font_cjk;
  tft.setFont(&g_font_cjk);
  int16_t x = (COVER_SIZE - tft.textWidth(s)) / 2;
  if (x < 0) x = 0;
  tft.setCursor(x, y);
  tft.print(s);
}

void circle_span(int y, int pad, int& x0, int& w)
{
  const int cx = COVER_SIZE / 2;   // 120
  const int cy = COVER_SIZE / 2;   // 120
  const int r  = COVER_SIZE / 2;   // 120

  int dy = y - cy;
  int ady = dy < 0 ? -dy : dy;
  if (ady >= r) { x0 = cx; w = 0; return; }

  float dx = sqrtf((float)(r * r - ady * ady));
  int xmin = (int)ceilf(cx - dx) + pad;
  int xmax = (int)floorf(cx + dx) - pad;

  if (xmax < xmin) { x0 = cx; w = 0; return; }
  x0 = xmin;
  w  = xmax - xmin + 1;
}

void fmt_mmss(uint32_t ms, char out[6])
{
  uint32_t total_sec = ms / 1000;
  uint32_t min = total_sec / 60;
  uint32_t sec = total_sec % 60;
  if (min > 99) min = 99;
  out[0] = '0' + (min / 10);
  out[1] = '0' + (min % 10);
  out[2] = ':';
  out[3] = '0' + (sec / 10);
  out[4] = '0' + (sec % 10);
  out[5] = '\0';
}

// 按像素宽度截断文本（末尾加 ...）
String clip_text_px(LGFX_Sprite* dst, const String& s, int max_w)
{
  if (!dst) return s;
  int full_w = dst->textWidth(s.c_str());
  if (full_w <= max_w) return s;

  const char* p = s.c_str();
  String out;
  int out_w = 0;
  int dots_w = dst->textWidth("...");
  int avail = max_w - dots_w;
  if (avail < 10) avail = 10;

  while (*p) {
    int clen = utf8_char_len((uint8_t)*p);
    String ch(p);
    ch.remove(clen);
    int cw = dst->textWidth(ch.c_str());
    if (out_w + cw > avail) break;
    out += ch;
    out_w += cw;
    p += clen;
  }
  out += "...";
  return out;
}

// UTF-8 像素级文本截断：按最大像素宽度截断，正确处理UTF-8字符（末尾加 ...）
String clip_utf8_by_px(LGFX_Sprite* dst, const String& s, int max_w)
{
  if (!dst) return s;
  
  // 先检查完整文本宽度
  int full_w = dst->textWidth(s.c_str());
  if (full_w <= max_w) return s;

  // 计算 ... 的宽度
  int dots_w = dst->textWidth("...");
  int avail = max_w - dots_w;
  if (avail < 10) avail = 10;  // 最小可用宽度

  String out;
  int out_w = 0;
  const char* p = s.c_str();
  
  while (*p) {
    // 获取UTF-8字符长度
    int clen = utf8_char_len((uint8_t)*p);
    if (clen < 1) clen = 1;
    
    // 提取单个字符
    String ch;
    for (int i = 0; i < clen && *p; i++) {
      ch += *p++;
    }
    
    // 计算字符宽度
    int cw = dst->textWidth(ch.c_str());
    
    // 如果加上这个字符会超出可用宽度，停止
    if (out_w + cw > avail) {
      break;
    }
    
    out += ch;
    out_w += cw;
  }
  
  out += "...";
  return out;
}

String clip_text(LGFX_Sprite* dst, const String& s, int max_w)
{
  return clip_text_px(dst, s, max_w);
}

void draw_title_with_note(LGFX_Sprite* dst,
                          int y,
                          const String& text,
                          int text_size,
                          uint16_t fg,
                          int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  extern void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return; // 需要足够空间显示图标+文字

  const int ICON_W = 14;
  const int ICON_H = 14;
  const int ICON_GAP = 2;

  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  int start_x = x0 + (w - total_w) / 2;

  int icon_y = y - ICON_H + 16;
  draw_note_icon_img(dst, start_x, icon_y, fg);

  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

void draw_artist_with_icon(LGFX_Sprite* dst,
                           int y,
                           const String& text,
                           int text_size,
                           uint16_t fg,
                           int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  extern void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return;

  const int ICON_W = 14;
  const int ICON_H = 14;
  const int ICON_GAP = 2;

  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  int start_x = x0 + (w - total_w) / 2;

  int icon_y = y - ICON_H + 16;
  draw_artist_icon_img(dst, start_x, icon_y, fg);

  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

void draw_album_with_icon(LGFX_Sprite* dst,
                          int y,
                          const String& text,
                          int text_size,
                          uint16_t fg,
                          int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  extern void draw_album_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);

  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 30) return;

  const int ICON_W = 14;
  const int ICON_H = 14;
  const int ICON_GAP = 2;

  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  int start_x = x0 + (w - total_w) / 2;

  int icon_y = y - ICON_H + 16;
  draw_album_icon_img(dst, start_x, icon_y, fg);

  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

// 在精灵上绘制居中文本（带颜色参数）
void draw_center_text_on_sprite(LGFX_Sprite* dst,
                                const char* s,
                                int y,
                                uint16_t fg,
                                int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  
  if (!dst || !s || s[0] == '\0') return;
  
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);
  
  // 计算圆屏安全区域
  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 10) return;
  
  // 截断文本以适应宽度
  String t = clip_utf8_by_px(dst, String(s), w);
  if (t.length() == 0) return;
  
  // 计算居中位置
  int tw = dst->textWidth(t.c_str());
  int x = x0 + (w - tw) / 2;
  if (x < x0) x = x0;
  
  // 绘制文本
  dst->setTextColor(fg);
  dst->setCursor(x, y);
  dst->print(t);
}
