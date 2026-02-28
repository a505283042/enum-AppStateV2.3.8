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
  uint32_t s = ms / 1000;
  uint32_t m = s / 60;
  s %= 60;
  out[0] = '0' + (m / 10) % 10;
  out[1] = '0' + (m % 10);
  out[2] = ':';
  out[3] = '0' + (s / 10);
  out[4] = '0' + (s % 10);
  out[5] = '\0';
}

String clip_text(LGFX_Sprite* dst, const String& s, int max_w)
{
  if (dst->textWidth(s.c_str()) <= max_w) return s;
  const String ell = "...";
  int ell_w = dst->textWidth(ell.c_str());
  if (ell_w >= max_w) return String("");

  String t = s;
  while (t.length() > 0 && dst->textWidth((t + ell).c_str()) > max_w) {
    t.remove(t.length() - 1);
  }
  return t + ell;
}

String clip_text_px(LGFX_Sprite* dst, const String& s, int max_w)
{
  if (max_w <= 0) return "";
  if (dst->textWidth(s.c_str()) <= max_w) return s;
  const String ell = "...";
  const int ell_w = dst->textWidth(ell.c_str());
  if (ell_w >= max_w) return "";
  String t = s;
  while (t.length() > 0 && dst->textWidth((t + ell).c_str()) > max_w) {
    t.remove(t.length() - 1);
  }
  return t + ell;
}

String clip_utf8_by_px(LGFX_Sprite* dst, const String& s, int max_w)
{
  if (max_w <= 0) return "";
  if (dst->textWidth(s.c_str()) <= max_w) return s;

  const String ell = "...";
  if (dst->textWidth(ell.c_str()) >= max_w) return "";

  const char* p = s.c_str();
  String out;

  for (int i = 0; p[i]; ) {
    int len = utf8_char_len((uint8_t)p[i]);
    out += String(p + i).substring(0, len);

    if (dst->textWidth((out + ell).c_str()) > max_w) {
      out.remove(out.length() - len);
      break;
    }
    i += len;
  }
  return out + ell;
}

// 原来的居中描边文本绘制函数（已注释掉，改用带图标的版本）
/*
void draw_text_center_outline(LGFX_Sprite* dst,
                           int y,
                           const String& text,
                           int text_size,
                           uint16_t fg,
                           int safe_pad)
{
  extern lgfx::U8g2font g_font_cjk;
  
  dst->setFont(&g_font_cjk);
  dst->setTextSize(text_size);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w <= 10) return;

  String t = clip_utf8_by_px(dst, text, w);
  if (t.length() == 0) return;

  int tw = dst->textWidth(t.c_str());
  int x = x0 + (w - tw) / 2;

  dst->setTextColor(fg);
  dst->setCursor(x, y);
  dst->print(t);
}
*/

// 绘制带音符图标的居中文字（歌曲名）
// 音符图标放在文字前面，整体居中
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
  const int ICON_GAP = 2; // 图标和文字之间的间距

  // 计算可用宽度（减去图标和间距）
  int text_max_w = w - ICON_W - ICON_GAP;
  if (text_max_w < 20) return;

  // 裁剪文字
  String t = clip_utf8_by_px(dst, text, text_max_w);
  if (t.length() == 0) return;

  // 计算总宽度（图标 + 间距 + 文字）
  int tw = dst->textWidth(t.c_str());
  int total_w = ICON_W + ICON_GAP + tw;

  // 计算起始位置（整体居中）
  int start_x = x0 + (w - total_w) / 2;

  // 绘制音符图标（垂直居中）
  int icon_y = y - ICON_H +16; // 调整图标垂直位置
  draw_note_icon_img(dst, start_x, icon_y, fg);

  // 绘制文字
  int text_x = start_x + ICON_W + ICON_GAP;
  dst->setTextColor(fg);
  dst->setCursor(text_x, y);
  dst->print(t);
}

// 绘制带歌手图标的居中文字（歌手名）
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

// 绘制带专辑图标的居中文字（专辑名）
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