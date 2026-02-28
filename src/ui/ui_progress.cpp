#include "ui/ui_progress.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_icons.h"
#include "ui/ui_colors.h"
#include "ui/ui.h"
#include "ui/ui_icon_images.h"

// 外部声明全局变量
extern lgfx::U8g2font g_font_cjk;
extern volatile uint8_t s_ui_volume;   // 0~100
extern volatile play_mode_t s_ui_play_mode;  // 播放模式
extern volatile int     s_ui_track_idx;  // 0-based
extern volatile int     s_ui_track_total;
extern String s_np_album;

void draw_time_bar(LGFX_Sprite* dst,
                  int y_bar, int y_time,
                  uint32_t el_ms,
                  uint32_t total_ms,        // 0=未知
                  int safe_pad,
                  uint16_t c_text)
{
  const uint16_t c_bar_bg   = UI_COLOR_BAR_BG;     // 未播（森林绿）
  const uint16_t c_bar_play = UI_COLOR_BAR_PLAY;   // 已播（暖阳橙黄）

  int x0, w;
  circle_span(y_bar, safe_pad, x0, w);
  if (w < 60) return;

  const int margin = 5;
  const int bar_x  = x0 + margin;
  const int bar_w  = w - margin * 2;
  const int bar_h  = 6;

  // 滑动点：有总时长就按比例走；未知则每分钟循环
  int dot = 0;
  
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) {
    uint32_t clamped = el_ms > total_ms ? total_ms : el_ms;
    float ratio = (float)clamped / (float)total_ms;
    if (ratio < 0.0f) ratio = 0.0f;
    if (ratio > 1.0f) ratio = 1.0f;
    dot = (int)(ratio * (float)(bar_w - 2));
  } else {
    dot = (int)((el_ms / 1000) % 60) * (bar_w - 2) / 60;
  }
  if (dot < 0) dot = 0;
  if (dot > bar_w - 2) dot = bar_w - 2;

  // 画进度条
  dst->fillRect(bar_x, y_bar, bar_w, bar_h, c_bar_bg);
  int played_w = dot + 1;
  if (played_w > 0) {
    dst->fillRect(bar_x + 1, y_bar + 1, played_w, bar_h - 2, c_bar_play);
  }

  // 画光标头（2像素宽）
  const uint16_t c_cursor = UI_COLOR_BAR_CURSOR;  // 活力紫红
  int cursor_x = bar_x + 1 + dot;
  dst->fillRect(cursor_x, y_bar, 2, bar_h, c_cursor);

  // 时间文本
  char el[6];
  fmt_mmss(el_ms, el);

  char total_str[6];
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) fmt_mmss(total_ms, total_str);
  else { memcpy(total_str, "--:--", 6); }

  dst->setTextSize(1);

  // 左侧 elapsed
  {
    int xL0, wL;
    circle_span(y_time, safe_pad, xL0, wL);
    int x = xL0 + 5;
    String s(el);

    // 无描边
    dst->setTextColor(c_text);
    dst->setCursor(x, y_time);
    dst->print(s);
  }

  // 右侧 total
  {
    circle_span(y_time, safe_pad, x0, w);
    int tw = dst->textWidth(total_str);
    int xr = x0 + w - tw - 5;

    // 无描边
    dst->setTextColor(c_text);
    dst->setCursor(xr, y_time);
    dst->print(total_str);
  }

  // ===== 时间行中间：曲目位置 idx/total =====
  char mid[16];
  if (s_ui_track_total > 0) snprintf(mid, sizeof(mid), "%d/%d", s_ui_track_idx + 1, s_ui_track_total);
  else                      snprintf(mid, sizeof(mid), "--/--");

  {
    int x0t, wt;
    circle_span(y_time, safe_pad, x0t, wt);

    int tw = dst->textWidth(mid);
    int xm = x0t + (wt - tw) / 2;

    // 简单防挤压：如果中间文本太宽就不画（一般不会）
    if (tw < wt - 80) {
      // 无描边
      dst->setTextColor(c_text);
      dst->setCursor(xm, y_time);
      dst->print(mid);
    }
  }
}

void draw_status_row(LGFX_Sprite* dst,
                    int y,
                    int safe_pad,
                    uint16_t fg,
                    bool volume_active)
{
  dst->setFont(&g_font_cjk);
  dst->setTextSize(1);

  int x0, w;
  circle_span(y, safe_pad, x0, w);
  if (w < 60) return;

  const int margin = 10;
  const int icon_size = 10;

  // 左侧：音量图标 + 百分比
  int xL = x0 + margin;
  uint16_t volume_color = volume_active ? UI_COLOR_VOLUME_ACTIVE : fg;
  draw_volume_icon(dst, xL, y - 5 + 10, volume_color);  // 11像素高，下移8像素
  
  char vol_str[8];
  snprintf(vol_str, sizeof(vol_str), "%u%%", (unsigned)s_ui_volume);
  dst->setTextColor(volume_color);
  dst->setCursor(xL + 11 + 4, y + 2);  // 音量图标宽度为11像素，文本下移2像素
  dst->print(vol_str);
  
  // 计算左侧音量区域的宽度
  int vol_width = 11 + 4 + dst->textWidth(vol_str);  // 图标 + 间距 + 文本

  // 右侧：播放模式图标（根据播放模式显示不同的图标组合）
  int xR = x0 + w - icon_size * 2 - margin * 2 + 7;  // 往右移7像素
  
  // 计算右侧模式图标区域的宽度
  int mode_width = icon_size * 2 + margin * 2 - 7;  // 两个图标 + 边距 - 右移7像素
  
  // 根据播放模式显示对应的图标
  switch (s_ui_play_mode) {
    case PLAY_MODE_ALL_SEQ:
      // 全部顺序：TF卡图标 + 顺序图标
      draw_tfcard_icon(dst, xR, y - icon_size / 2 + 8, fg);
      draw_repeat_icon(dst, xR + icon_size + 4, y - icon_size / 2 + 8, fg);
      break;
      
    case PLAY_MODE_ALL_RND:
      // 全部随机：TF卡图标 + 随机图标
      draw_tfcard_icon(dst, xR, y - icon_size / 2 + 8, fg);
      draw_random_icon(dst, xR + icon_size + 4, y - icon_size / 2 + 8, fg);
      break;
      
    case PLAY_MODE_ARTIST_SEQ:
      // 歌手顺序：歌手图标 + 顺序图标
      draw_artist_icon(dst, xR, y - icon_size / 2 + 8, fg);
      draw_repeat_icon(dst, xR + icon_size + 4, y - icon_size / 2 + 8, fg);
      break;
      
    case PLAY_MODE_ARTIST_RND:
      // 歌手随机：歌手图标 + 随机图标
      draw_artist_icon(dst, xR, y - icon_size / 2 + 8, fg);
      draw_random_icon(dst, xR + icon_size + 4, y - icon_size / 2 + 8, fg);
      break;
      
    case PLAY_MODE_ALBUM_SEQ:
      // 专辑顺序：专辑图标 + 顺序图标
      draw_album_icon(dst, xR, y - icon_size / 2 + 8, fg);
      draw_repeat_icon(dst, xR + icon_size + 4, y - icon_size / 2 + 8, fg);
      break;
      
    case PLAY_MODE_ALBUM_RND:
      // 专辑随机：专辑图标 + 随机图标
      draw_album_icon(dst, xR, y - icon_size / 2 + 8, fg);
      draw_random_icon(dst, xR + icon_size + 4, y - icon_size / 2 + 8, fg);
      break;
  }

  // 中间：专辑名（带专辑图标，空就给个占位）
  // 原来的绘制方法（已注释掉）：
  // String midS = s_np_album;
  // if (midS.length() == 0) midS = "未知专辑";
  // int available_width = w - vol_width - mode_width - margin;
  // int twM = dst->textWidth(midS.c_str());
  // if (twM > available_width) {
  //   int ellipsis_width = dst->textWidth("...");
  //   while ((twM + ellipsis_width) > available_width && midS.length() > 0) {
  //     midS = midS.substring(0, midS.length() - 1);
  //     twM = dst->textWidth(midS.c_str());
  //   }
  //   midS += "...";
  //   twM += ellipsis_width;
  // }
  // int xM = xL + vol_width + (available_width - twM) / 2;
  // dst->setTextColor(UI_COLOR_ALBUM);
  // dst->setCursor(xM, y);
  // dst->print(midS.c_str());

  // 新的绘制方法（带专辑图标）：
  String midS = s_np_album;
  if (midS.length() == 0) midS = "未知专辑";

  // 计算中间可用空间（音量区域结束位置到模式图标开始位置）
  int available_width = w - vol_width - mode_width - margin;

  const int ALBUM_ICON_W = 14;
  const int ALBUM_ICON_GAP = 2;

  // 计算专辑名文本最大可用宽度（减去图标和间距）
  int max_text_width = available_width - ALBUM_ICON_W - ALBUM_ICON_GAP;

  // 使用 clip_utf8_by_px 进行裁断（参考之前的策略）
  midS = clip_utf8_by_px(dst, midS, max_text_width);

  // 计算实际宽度
  int twM = dst->textWidth(midS.c_str());
  int total_width = ALBUM_ICON_W + ALBUM_ICON_GAP + twM;

  // 居中显示（图标+文字整体居中）
  int start_x = xL + vol_width + (available_width - total_width) / 2;

  // 绘制专辑图标
  int icon_y = y - ALBUM_ICON_W + 15; // 调整垂直位置
  draw_album_icon_img(dst, start_x, icon_y, UI_COLOR_ALBUM);

  // 绘制专辑名
  int text_x = start_x + ALBUM_ICON_W + ALBUM_ICON_GAP;
  dst->setTextColor(UI_COLOR_ALBUM);  // 专辑名颜色（中灰）
  dst->setCursor(text_x, y);
  dst->print(midS.c_str());
}