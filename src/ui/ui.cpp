#include <Arduino.h>
#include "board/board_pins.h"
#include "ui/ui.h"
#include "utils/log.h"
#undef LOG_TAG
#define LOG_TAG "UI"

#include <math.h>

#include "ui/gc9a01_lgfx.h"
#include "storage/storage_music.h"
#include "ui/ui_cover_mem.h"
#include "audio/audio.h"
#include "ui/ui_icons.h"
#include "ui/ui_text_utils.h"
#include "ui/ui_progress.h"
#include "ui/ui_colors.h"

#include <SdFat.h>
#include <esp32-hal-psram.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#include <lgfx/v1/lgfx_fonts.hpp>
#include "fonts/u8g2_font_wenquanyi_merged.h"

lgfx::U8g2font g_font_cjk(u8g2_font_wenquanyi_merged);

static TaskHandle_t s_ui_task = nullptr;
static SemaphoreHandle_t s_ui_mtx = nullptr;

static inline void ui_lock()   { if (s_ui_mtx) xSemaphoreTakeRecursive(s_ui_mtx, portMAX_DELAY); }
static inline void ui_unlock() { if (s_ui_mtx) xSemaphoreGiveRecursive(s_ui_mtx); }

extern SdFat sd;

// =============================================================================
// 最小化 UI（干净重构）
// - BOOT / SCAN：简单文本 + 扫描点
// - PLAYER：使用双缓冲精灵的全屏旋转专辑封面
// - 默认稳定：pushSprite()（无 DMA）。这避免了当精灵位于 PSRAM 时出现"花屏"问题。
//   您可以稍后使用支持 DMA 的反弹缓冲区重新启用 DMA。
// =============================================================================

// ---------------- 屏幕状态 ----------------
static ui_screen_t s_screen = UI_SCREEN_BOOT;
LGFX tft;

// ---------------- 封面精灵 ----------------
static constexpr int   COVER_SIZE        = 240;
static constexpr float COVER_DEG_PER_SEC = 15.0f;  // 封面旋转速度：每秒旋转  度

// FPS policy
static constexpr uint32_t UI_FPS_ROTATE = 20; // 旋转视图
static constexpr uint32_t UI_FPS_INFO   = 5;  // 信息视图
static constexpr uint32_t UI_FPS_OTHER  = 1;  // 其它界面

static volatile enum ui_player_view_t s_view = UI_VIEW_INFO;
static String s_np_title;
static String s_np_artist;
String s_np_album;

// ===== status row (above progress bar) =====
volatile uint8_t s_ui_volume = 100;   // 0~100
volatile play_mode_t s_ui_play_mode = PLAY_MODE_ALL_SEQ;  // 播放模式
volatile int     s_ui_track_idx = 0;  // 0-based
volatile int     s_ui_track_total = 0;

// 音量激活状态
static volatile uint32_t s_ui_volume_active_time = UINT32_MAX;  // 音量激活时间戳（初始化为最大值，确保启动时不显示激活状态）
#define VOLUME_ACTIVE_TIMEOUT_MS 2000  // 音量激活超时时间（毫秒）

// 播放模式切换高亮状态
volatile uint32_t s_ui_mode_switch_time = 0;  // 模式切换时间戳
#define MODE_SWITCH_HIGHLIGHT_MS 2000  // 模式切换高亮时间（毫秒）

// --- 播放进度（由播放层喂给 UI）---
static volatile uint32_t s_ui_play_ms  = 0;
static volatile uint32_t s_ui_total_ms = 0;  // 0=未知

static LGFX_Sprite s_coverSpr(&tft);
static bool s_coverSprInited = false;
static bool s_coverSprReady  = false;

static LGFX_Sprite s_frame0(&tft);
static LGFX_Sprite s_frame1(&tft);
static LGFX_Sprite* s_frame[2] = { &s_frame0, &s_frame1 };
static uint8_t s_front = 0;
static uint8_t s_back  = 1;
static bool s_framesInited = false;

static LGFX_Sprite* s_src = nullptr;

static float s_angle_deg = 0.0f;
static uint32_t s_rot_last_ms = 0;

// ---------------- 扫描 UI 状态 ----------------
static uint32_t s_scan_last_ms = 0;
static int s_scan_phase = 0;

static volatile bool s_ui_hold = false;

void ui_hold_render(bool hold)
{
  s_ui_hold = hold;
  // 进入 hold 时刷新 rot 时钟，避免解除 hold 后角度跳变
  if (hold) s_rot_last_ms = millis();
}

// ---------------- 辅助函数 ----------------

// 从 PNG 数据中获取图像宽度和高度
// 参数: data - PNG 数据指针, len - 数据长度, w/h - 输出宽度和高度
// 返回: 成功返回 true，失败返回 false
static bool png_get_wh(const uint8_t* data, size_t len, int& w, int& h);

// 从 JPEG 数据中获取图像宽度和高度
// 参数: data - JPEG 数据指针, len - 数据长度, w/h - 输出宽度和高度
// 返回: 成功返回 true，失败返回 false
static bool jpg_get_wh(const uint8_t* data, size_t len, int& w, int& h);

static bool png_get_wh(const uint8_t* data, size_t len, int& w, int& h)
{
  // 检查数据有效性
  if (!data || len < 24) return false;
  
  // 验证 PNG 签名 (8 字节魔数)
  const uint8_t sig[8] = {0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A};
  for (int i = 0; i < 8; ++i) if (data[i] != sig[i]) return false;

  // 从 IHDR 块中提取宽度和高度 (大端序，各占 4 字节)
  w = (int)((uint32_t)data[16] << 24 | (uint32_t)data[17] << 16 | (uint32_t)data[18] << 8 | (uint32_t)data[19]);
  h = (int)((uint32_t)data[20] << 24 | (uint32_t)data[21] << 16 | (uint32_t)data[22] << 8 | (uint32_t)data[23]);
  
  // 验证尺寸有效性
  return (w > 0 && h > 0);
}

static bool jpg_get_wh(const uint8_t* data, size_t len, int& w, int& h)
{
  // 检查数据有效性
  if (!data || len < 4) return false;

  // 验证 JPEG SOI (Start of Image) 标记
  if (data[0] != 0xFF || data[1] != 0xD8) return false; // SOI

  size_t i = 2;

  // 判断是否为 SOF (Start of Frame) 标记
  auto is_sof = [](uint8_t m) {
    return (m==0xC0||m==0xC1||m==0xC2||m==0xC3||m==0xC5||m==0xC6||m==0xC7||
            m==0xC9||m==0xCA||m==0xCB||m==0xCD||m==0xCE||m==0xCF);
  };

  // 遍历 JPEG 段寻找 SOF 标记
  while (i + 3 < len) {
    // 跳过非 0xFF 字节
    if (data[i] != 0xFF) { ++i; continue; }

    // 跳过填充的 0xFF 字节
    while (i < len && data[i] == 0xFF) ++i;
    if (i >= len) break;

    // 读取标记字节
    uint8_t marker = data[i++];

    // 遇到 SOS (Start of Scan) 或 EOI (End of Image) 则停止
    if (marker == 0xDA || marker == 0xD9) break; // SOS / EOI

    // 读取段长度 (大端序，2 字节)
    if (i + 1 >= len) break;
    uint16_t seglen = (uint16_t)((data[i] << 8) | data[i + 1]);
    if (seglen < 2) return false;
    if (i + seglen > len) return false;

    // 找到 SOF 标记，提取图像尺寸
    if (is_sof(marker)) {
      size_t p = i + 2;
      if (p + 4 >= len) return false;
      // SOF 中高度在前，宽度在后 (各占 2 字节)
      h = (int)((data[p + 1] << 8) | data[p + 2]);
      w = (int)((data[p + 3] << 8) | data[p + 4]);
      // 验证尺寸有效性
      return (w > 0 && h > 0);
    }

    // 跳过当前段
    i += seglen;
  }
  return false;
}

static inline uint8_t _c5to8(uint8_t v) { return (v << 3) | (v >> 2); }
static inline uint8_t _c6to8(uint8_t v) { return (v << 2) | (v >> 4); }
static inline uint16_t _rgb888_to_565(uint8_t r, uint8_t g, uint8_t b)
{
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// crop=true：居中裁切填满（推荐）
// crop=false：等比缩放留黑边
static void cover_downscale_bilinear(LGFX_Sprite& src, LGFX_Sprite& dst, int dstSize, bool crop)
{
  int sw = src.width();
  int sh = src.height();
  if (sw <= 0 || sh <= 0) return;

  float sx = (float)dstSize / (float)sw;
  float sy = (float)dstSize / (float)sh;
  float s  = crop ? (sx > sy ? sx : sy) : (sx < sy ? sx : sy);

  float regionW = (float)dstSize / s;
  float regionH = (float)dstSize / s;
  float startX  = ((float)sw - regionW) * 0.5f;
  float startY  = ((float)sh - regionH) * 0.5f;

  float stepX = regionW / (float)dstSize;
  float stepY = regionH / (float)dstSize;

  dst.fillScreen(TFT_BLACK);

  for (int y = 0; y < dstSize; ++y) {
    float fy = startY + (y + 0.5f) * stepY - 0.5f;
    int y0 = (int)floorf(fy);
    float ty = fy - y0;
    if (y0 < 0) { y0 = 0; ty = 0; }
    int y1 = y0 + 1; if (y1 >= sh) y1 = sh - 1;

    for (int x = 0; x < dstSize; ++x) {
      float fx = startX + (x + 0.5f) * stepX - 0.5f;
      int x0 = (int)floorf(fx);
      float tx = fx - x0;
      if (x0 < 0) { x0 = 0; tx = 0; }
      int x1 = x0 + 1; if (x1 >= sw) x1 = sw - 1;

      uint16_t c00 = src.readPixel(x0, y0);
      uint16_t c10 = src.readPixel(x1, y0);
      uint16_t c01 = src.readPixel(x0, y1);
      uint16_t c11 = src.readPixel(x1, y1);

      // RGB565 -> RGB888
      uint8_t r00 = _c5to8((c00 >> 11) & 0x1F), g00 = _c6to8((c00 >> 5) & 0x3F), b00 = _c5to8(c00 & 0x1F);
      uint8_t r10 = _c5to8((c10 >> 11) & 0x1F), g10 = _c6to8((c10 >> 5) & 0x3F), b10 = _c5to8(c10 & 0x1F);
      uint8_t r01 = _c5to8((c01 >> 11) & 0x1F), g01 = _c6to8((c01 >> 5) & 0x3F), b01 = _c5to8(c01 & 0x1F);
      uint8_t r11 = _c5to8((c11 >> 11) & 0x1F), g11 = _c6to8((c11 >> 5) & 0x3F), b11 = _c5to8(c11 & 0x1F);

      // bilinear
      float w00 = (1 - tx) * (1 - ty);
      float w10 = (tx)     * (1 - ty);
      float w01 = (1 - tx) * (ty);
      float w11 = (tx)     * (ty);

      uint8_t r = (uint8_t)(r00*w00 + r10*w10 + r01*w01 + r11*w11 + 0.5f);
      uint8_t g = (uint8_t)(g00*w00 + g10*w10 + g01*w01 + g11*w11 + 0.5f);
      uint8_t b = (uint8_t)(b00*w00 + b10*w10 + b01*w01 + b11*w11 + 0.5f);

      dst.drawPixel(x, y, _rgb888_to_565(r, g, b));
    }
  }
}

// 将封面图像缩放并渲染到 240x240 的精灵中
// 参数: ptr - 图像数据指针, len - 数据长度, is_png - 是否为 PNG 格式
// 返回: 成功返回 true，失败返回 false
// 功能: 先获取原图尺寸，然后创建临时精灵解码原图，最后缩放到 240x240 并居中裁切填满
static bool cover_blit_scaled_to_240(const uint8_t* ptr, size_t len, bool is_png)
{
  // 获取原图尺寸
  int srcW = 0, srcH = 0;
  bool got = is_png ? png_get_wh(ptr, len, srcW, srcH) : jpg_get_wh(ptr, len, srcW, srcH);
  if (!got || srcW <= 0 || srcH <= 0) {
    // 拿不到尺寸就退回"裁剪模式"（直接绘制到 240x240）
    bool ok = false;
    s_coverSpr.fillScreen(TFT_BLACK);
    if (is_png) ok = s_coverSpr.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    else        ok = s_coverSpr.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    return ok;
  }

  // 安全阈值：防止超大封面炸内存
  if (srcW > 800 || srcH > 800) {
    // 太大就不要创建超大 tmp，直接走原来的裁剪绘制
    s_coverSpr.fillScreen(TFT_BLACK);
    bool ok = is_png ? s_coverSpr.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE)
                     : s_coverSpr.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    return ok;
  }

  // 1) 创建临时 sprite：解码原图（800x800 约 1.9MB，PSRAM 足够）
  LGFX_Sprite tmp(&tft);

  // 用 16bpp 足够做缩放，内存压力小很多（600x600 从 ~1.0MB 降到 ~0.7MB）
  tmp.setColorDepth(16);
  tmp.setPsram(psramFound());

  // ✅ 关键：必须检查是否创建成功
  if (!tmp.createSprite(srcW, srcH)) {
    LOGW("[COVER] tmp.createSprite(%d,%d) failed -> fallback crop", srcW, srcH);

    // 创建失败就退回“直接画到 240x240”（能显示就行，别崩）
    s_coverSpr.fillScreen(TFT_BLACK);
    bool ok = is_png ? s_coverSpr.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE)
                    : s_coverSpr.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    return ok;
  }

  tmp.fillScreen(TFT_BLACK);;

  // 解码原图到临时 sprite
  bool ok = false;
  if (is_png) ok = tmp.drawPng(ptr, len, 0, 0);   // 注意：这里不要给 240x240，否则又会裁剪
  else        ok = tmp.drawJpg(ptr, len, 0, 0);

  if (!ok) {
    tmp.deleteSprite();
    return false;
  }

  // 将缩放后的图像绘制到封面精灵中
  s_coverSpr.fillScreen(TFT_BLACK);
  cover_downscale_bilinear(tmp, s_coverSpr, COVER_SIZE, true); // true=居中裁切填满

  // 释放临时 sprite
  tmp.deleteSprite();
  return true;
}

// 前向声明
static void cover_sprite_init_once();

static void cover_draw_placeholder(const char* msg)
{
  cover_sprite_init_once();
  s_coverSpr.fillScreen(TFT_BLACK);
  s_coverSpr.drawRect(0, 0, COVER_SIZE, COVER_SIZE, TFT_DARKGREY);

  s_coverSpr.setTextSize(2);
  s_coverSpr.setTextColor(TFT_WHITE);

  const char* s = (msg && msg[0]) ? msg : "NO COVER";
  int16_t x = (COVER_SIZE - s_coverSpr.textWidth(s)) / 2;
  if (x < 0) x = 0;
  s_coverSpr.setCursor(x, 110);
  s_coverSpr.print(s);
}

// 初始化封面精灵（仅执行一次）
// 功能: 创建 240x240 的封面精灵，用于存储解码后的封面图像
//      如果检测到 PSRAM，则使用 PSRAM 以节省内部 RAM
static void cover_sprite_init_once()
{
  // 如果已经初始化过，直接返回
  if (s_coverSprInited) return;

  // 检测 PSRAM 并配置精灵
  const bool has_psram = psramFound();
  s_coverSpr.setColorDepth(16);
  s_coverSpr.setPsram(has_psram);
  // 创建 240x240 的精灵
  s_coverSpr.createSprite(COVER_SIZE, COVER_SIZE);
  s_coverSpr.fillScreen(TFT_BLACK);
  s_coverSprInited = true;

  // === Step 2: 添加半透明遮罩
  // 使用 fillRectAlpha 方法创建半透明黑色遮罩
  // alpha=128 表示半透明
  s_coverSpr.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
}

// 初始化双缓冲帧精灵（仅执行一次）
// 功能: 创建两个 240x240 的精灵用于双缓冲，实现无闪烁的旋转封面动画
//      如果检测到 PSRAM，则使用 PSRAM 以节省内部 RAM
static void cover_frames_init_once()
{
  // 如果已经初始化过，直接返回
  if (s_framesInited) return;

  // 检测 PSRAM 并配置两个帧精灵
  const bool has_psram = psramFound();
  for (int i = 0; i < 2; ++i) {
    s_frame[i]->setColorDepth(16);
    s_frame[i]->setPsram(has_psram);
    // 创建 240x240 的精灵
    s_frame[i]->createSprite(COVER_SIZE, COVER_SIZE);
    s_frame[i]->fillScreen(TFT_BLACK);
  }

  // 初始化前后帧索引
  s_front = 0;
  s_back  = 1;
  s_framesInited = true;
}

// 设置封面旋转动画的源精灵
// 参数: src - 源精灵指针（通常是 s_coverSpr）
// 功能: 指定要旋转的封面精灵，后续的旋转操作将基于此精灵
static void cover_set_source(LGFX_Sprite* src)
{
  s_src = src;
}

// 将旋转的封面渲染到后帧并推送到 LCD（稳定路径）
// 参数: angle_deg - 旋转角度（度）
// 功能: 将源精灵旋转指定角度后绘制到后帧，然后推送到屏幕
//      使用双缓冲技术避免闪烁，交换前后帧索引
static void cover_rotate_draw(float angle_deg)
{
  // 检查帧是否已初始化以及源精灵是否有效
  if (!s_framesInited || !s_src) return;

  // 获取后帧指针
  auto* dst = s_frame[s_back];
  // 清空后帧
  dst->fillScreen(TFT_BLACK);
  // 将源精灵旋转指定角度并绘制到后帧（不缩放）
  s_src->pushRotateZoom(dst, COVER_SIZE / 2, COVER_SIZE / 2, angle_deg, 1.0f, 1.0f);

  // 将后帧推送到屏幕 (0, 0) 位置
  dst->pushSprite(0, 0);

  // 交换前后帧索引（双缓冲）
  uint8_t tmp = s_front;
  s_front = s_back;
  s_back = tmp;
}

static void cover_info_draw()
{
  if (!s_framesInited) return;

  auto* dst = s_frame[s_back];
  dst->fillScreen(TFT_BLACK);

  // 1) 静态封面（整屏）- 带半透明遮罩
  if (s_coverSprReady) {
    // 使用未使用的帧缓冲作为临时精灵，避免额外内存使用
    int temp_frame = (s_back == 0) ? 1 : 0; // 使用另一个缓冲
    LGFX_Sprite* temp_sprite = s_frame[temp_frame];
    
    // 绘制封面到临时精灵
    temp_sprite->fillScreen(TFT_BLACK);
    s_coverSpr.pushSprite(temp_sprite, 0, 0);
    // 添加半透明遮罩到临时精灵
    temp_sprite->fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 160, TFT_BLACK);
    // 一次性推送到目标精灵
    temp_sprite->pushSprite(dst, 0, 0);
  }

  // 2) 参数：圆屏安全边距 + 更大的字号 + 更紧凑的排布
  const int safe_pad = 12;

  const uint16_t c_title  = UI_COLOR_TITLE;   // 歌名文字颜色（纯白）
  const uint16_t c_artist = UI_COLOR_ARTIST;  // 歌手文字颜色（浅灰）

  // 3) 把信息区抬高一点，避免圆屏底部变窄导致左右被裁

  const int y_status = 131;  // 状态栏（音量/模式/列表）上移1像素
  const int y_bar   = 149;   // 进度条下移1像素
  const int y_time  = 157;   // 时间（上移3像素）
  const int y_title = 176;   // 标题
  const int y_artist= 195;   // 歌手（下移3像素）

  // 时间显示颜色常量
  const uint16_t c_time_text = UI_COLOR_TIME;  // 时间文字颜色（浅灰）

  // 4) 进度条（显示 elapsed + total）
  uint32_t el_ms = audio_get_play_ms();
  uint32_t total_ms = audio_get_total_ms();

  char total_str[6];
  if (total_ms >= 1000 && total_ms != 0xFFFFFFFFu) {
    fmt_mmss(total_ms, total_str);
  } else {
    memcpy(total_str, "--:--", 6); // 包含 '\0'
  }

  // 检查音量是否处于激活状态
  bool volume_active = (millis() - s_ui_volume_active_time) < VOLUME_ACTIVE_TIMEOUT_MS;
  draw_status_row(dst, y_status, safe_pad, c_time_text, volume_active);

  draw_time_bar(dst,
                y_bar, y_time,
                el_ms,
                total_ms,
                safe_pad,
                c_time_text);

  // 5) 标题（带音符图标）/歌手（带歌手图标）+ 描边外扩
  draw_title_with_note(dst, y_title, s_np_title, 1, c_title, safe_pad);
  draw_artist_with_icon(dst, y_artist, s_np_artist, 1, c_artist, safe_pad);
  // 原来的绘制方法（已注释掉）：
  // draw_text_center_outline(dst, y_title,  s_np_title,  1, c_title,  safe_pad);
  // draw_text_center_outline(dst, y_artist, s_np_artist, 1, c_artist, safe_pad);

  // 6) 推屏
  dst->pushSprite(0, 0);

  uint8_t tmp = s_front;
  s_front = s_back;
  s_back  = tmp;
}

static inline TickType_t ui_period_ticks()
{
  // hold 期间：不画，但要“醒得勤快一点”，保证解除 hold 后立刻恢复（这里按旋转帧率）
  if (s_ui_hold) return pdMS_TO_TICKS(1000 / UI_FPS_ROTATE);

  // PLAYER 界面：按视图区分帧率
  if (s_screen == UI_SCREEN_PLAYER) {
    if (s_view == UI_VIEW_ROTATE) return pdMS_TO_TICKS(1000 / UI_FPS_ROTATE);
    return pdMS_TO_TICKS(1000 / UI_FPS_INFO); // UI_VIEW_INFO 或其它默认走 info
  }

  // 其它界面：1fps
  return pdMS_TO_TICKS(1000 / UI_FPS_OTHER);
}

static void ui_task_entry(void*)
{
  TickType_t lastWake   = xTaskGetTickCount();
  TickType_t lastPeriod = 0;

  for (;;) {
    const uint32_t now_ms = millis();

    // 动态帧率：rotate 20fps / info 5fps / other 1fps
    TickType_t period = ui_period_ticks();
    if (period == 0) period = 1;

    // period 改变时重置基准，避免 vTaskDelayUntil 抖动
    if (period != lastPeriod) {
      lastWake = xTaskGetTickCount();
      lastPeriod = period;
    }

    // hold：不渲染，但刷新 rot 时钟，避免解除 hold 后 dt 巨大导致角度跳变
    if (s_ui_hold) {
      s_rot_last_ms = now_ms;
      vTaskDelayUntil(&lastWake, period);
      continue;
    }

    // 只在 PLAYER 界面、封面就绪时推屏
    if (s_screen == UI_SCREEN_PLAYER && s_coverSprReady && s_framesInited) {
      ui_lock();

      if (s_view == UI_VIEW_ROTATE && s_src) {
        if (s_rot_last_ms == 0) s_rot_last_ms = now_ms;

        float dt = (now_ms - s_rot_last_ms) * 0.001f;
        s_rot_last_ms = now_ms;

        // 防止任何阻塞导致 dt 过大（看起来像“后台一直在转”）
        if (dt > 0.20f) dt = 0.20f;

        s_angle_deg += COVER_DEG_PER_SEC * dt;
        if (s_angle_deg >= 360.0f) s_angle_deg -= 360.0f;

        cover_rotate_draw(s_angle_deg);
      } else {
        // INFO：显示正向封面+信息，同时刷新 rot 时钟，回到旋转不跳角度
        s_rot_last_ms = now_ms;
        cover_info_draw();
      }

      ui_unlock();
    } else {
      // 非 PLAYER：也刷新 rot 时钟，避免回到旋转 dt 累积
      s_rot_last_ms = now_ms;
    }

    vTaskDelayUntil(&lastWake, period);
  }
}

static void ui_task_start_once()
{
  if (s_ui_task) return;

  if (!s_ui_mtx) s_ui_mtx = xSemaphoreCreateRecursiveMutex();

  // UiTask 固定 core1，低优先级（比音频低很多）
  xTaskCreatePinnedToCore(
    ui_task_entry,
    "UiTask",
    4096,
    nullptr,
    1,      // 低优先级
    &s_ui_task,
    1       // core1
  );
}

// ---------------- 封面解码（每次曲目切换仅从 SD 读取一次） ----------------

// 从曲目信息中解码封面图像到精灵
// 参数: t - 曲目信息结构体
// 返回: 成功返回 true，失败返回 false
// 功能: 从 SD 卡读取曲目文件中的封面图像，解码并缩放到 240x240
//      如果没有封面，则使用默认封面
static bool cover_decode_to_sprite_from_track(const TrackInfo& t)
{
  // 初始化封面精灵
  cover_sprite_init_once();

  const uint8_t* ptr = nullptr;
  size_t len = 0;
  bool is_png = false;

  // 尝试从曲目文件中加载封面到内存
  if (!cover_load_to_memory(sd, t, ptr, len, is_png)) {
  
  // 默认封面缺失 “画占位并当作成功”
    if (!sd.exists("/System/default_cover.jpg")) {
    LOGW("[COVER] default cover not found -> placeholder");
    ui_lock();
    cover_draw_placeholder("NO COVER");
    s_coverSprReady = true;
    ui_unlock();
    return true;
  }

    // 打开默认封面文件
    File32 f = sd.open("/System/default_cover.jpg", O_RDONLY);
    if (!f) {
      LOGW("[COVER] open default cover failed");
      s_coverSprReady = false;
      return false;
    }

    // 读取文件大小
    uint32_t fileSize = f.fileSize();
    if (fileSize == 0 || !cover_ensure_buffer(fileSize)) {
      f.close();
      LOGW("[COVER] ensure buffer failed for default cover");
      s_coverSprReady = false;
      return false;
    }

    // 读取文件内容到缓冲区
    uint8_t* buf = cover_get_buffer();
    int bytesRead = f.read(buf, fileSize);
    f.close();
    if (bytesRead != (int)fileSize) {
      LOGW("[COVER] read default cover failed");
      s_coverSprReady = false;
      return false;
    }

    // 将默认封面缩放到 240x240
    ui_lock();
    bool ok = cover_blit_scaled_to_240(buf, fileSize, false);

    s_coverSprReady = ok;
    LOGI("[COVER] default cover ok=%d", (int)ok);
    ui_unlock();
    return ok;
  }

  // 使用 MIME 类型判断图像格式（比猜测更可靠）
  if (t.cover_mime == "image/png") is_png = true;
  else if (t.cover_mime == "image/jpeg") is_png = false;

  // 将封面图像缩放到 240x240
  ui_lock();
  bool ok = cover_blit_scaled_to_240(ptr, len, is_png);

  s_coverSprReady = ok;
  LOGI("[COVER] decode to sprite ok=%d", (int)ok);
  ui_unlock();
  return ok;
}

// =============================================================================
// 公共 UI API (ui.h)
// =============================================================================

void ui_init(void)
{
  LOGI("[UI] init (LGFX GC9A01)");

  ui_lock();
  tft.init();
  tft.setRotation(0);

  tft.initDMA();
  LOGI("[UI] DMA initialized");

  cover_sprite_init_once();
  cover_frames_init_once();

  ui_task_start_once();

  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("ESP32 Player", 90);
  tft.setTextSize(1);
  draw_center_text("启动中...", 130);

  s_screen = UI_SCREEN_BOOT;
  ui_unlock();
}

void ui_set_screen(ui_screen_t screen)
{
  s_screen = screen;
  LOGI("[UI] switch screen -> %d", (int)screen);
}

void ui_update(void)
{
  // UiTask 已负责封面旋转推屏，此函数已废弃
  // 保留此函数以保持 API 兼容性
}

void ui_show_message(const char* msg)
{
  if (!msg) msg = "";
  LOGI("[UI] message: %s", msg);

  ui_lock();
  // 底部提示（对当前全屏封面 UI 安全）
  tft.fillRect(0, 200, 240, 40, TFT_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  draw_center_text(msg, 220);
  ui_unlock();
}

void ui_enter_boot(void)
{
  ui_lock();
  ui_set_screen(UI_SCREEN_BOOT);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("BOOT", 90);
  tft.setTextSize(1);
  tft.setTextColor(TFT_GREEN, TFT_BLACK);
  draw_center_text("Mount SD / Scan MUSIC", 130);
  ui_unlock();
}

void ui_enter_player(void)
{
  ui_lock();
  ui_set_screen(UI_SCREEN_PLAYER);
  
  // ✅ 关键：先禁止 UiTask 使用旧封面刷屏，避免闪一下上一首
  s_coverSprReady = false;
  s_src = nullptr;
  s_angle_deg = 0.0f;
  s_rot_last_ms = 0;

  // 重置进度（新进入播放器/切歌时由播放层重新喂入）
  s_ui_play_ms  = 0;
  s_ui_total_ms = 0;

  tft.fillScreen(TFT_BLACK);

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_unlock();
}

void ui_enter_nfc_admin(void)
{
  ui_lock();
  ui_set_screen(UI_SCREEN_NFC_ADMIN);
  tft.fillScreen(TFT_BLACK);
  tft.setTextSize(2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  draw_center_text("NFC ADMIN", 90);
  ui_unlock();
}

bool ui_draw_cover_for_track(const TrackInfo& t, bool force_redraw)
{
  static String last_sig;
  String sig = t.audio_path + "#" + String((unsigned)t.cover_offset) + "#" + String((unsigned)t.cover_size);
  if (!force_redraw && sig == last_sig) return true;
  last_sig = sig;

  bool ok = cover_decode_to_sprite_from_track(t);
  if (!ok) return false;

  ui_lock();
  cover_set_source(&s_coverSpr);
  s_angle_deg = 0.0f;
  s_rot_last_ms = 0;   // 让 UiTask 下次自己初始化 dt

  // 切歌：先清空总时长，等待播放层重新喂入
  s_ui_play_ms  = 0;
  s_ui_total_ms = 0;

  // 立即推送第一帧
  tft.fillScreen(TFT_BLACK);
  s_coverSpr.pushSprite(0, 0);

  s_angle_deg = 0.0f;
  s_rot_last_ms = millis();
  ui_unlock();

  return true;
}

// =============================================================================
// 可选的覆盖层 API（目前保持为轻量级存根）
// =============================================================================
void ui_player_draw_overlay(const TrackInfo&, uint32_t, uint32_t,
                            const char*, const char*, const char*)
{
  // 最小化构建：无覆盖层
}

void ui_player_update_progress(uint32_t play_ms, uint32_t total_ms)
{
  s_ui_play_ms  = play_ms;
  s_ui_total_ms = total_ms;   // 0 表示未知
}

void ui_player_update_lyrics(const char*, const char*)
{
  // 最小化构建：无覆盖层
}

void ui_show_scanning()
{
  ui_scan_begin();
}

// =============================================================================
// 扫描 UI（由 storage_music.cpp / keys.cpp 使用）
// =============================================================================

void ui_scan_begin()
{
  ui_lock();
  ui_set_screen(UI_SCREEN_BOOT);

  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);

  tft.setTextSize(2);
  draw_center_text("正在扫描", 60);

  tft.setTextSize(1);
  draw_center_text("请稍候", 100);

  // 清除动画和计数区域
  tft.fillRect(0, 130, 240, 110, TFT_BLACK);

  s_scan_last_ms = 0;
  s_scan_phase = 0;
  ui_unlock();
}

// 绘制扫描动画的点
// 参数: phase - 当前动画阶段 (0, 1, 2)
// 功能: 绘制三个点，根据 phase 参数高亮显示其中一个点，形成动画效果
static void draw_scan_dots(int phase)
{
  // 计算点的中心位置和间距
  int cx = 120;
  int y  = 155;
  int dx = 18;

  // 清除点区域
  tft.fillRect(cx - 40, y - 12, 80, 24, TFT_BLACK);

  // 绘制三个点
  for (int i = 0; i < 3; i++) {
    int x = cx + (i - 1) * dx;
    // 当前阶段填充圆点，其他阶段绘制空心圆点
    if (i == phase) tft.fillCircle(x, y, 5, TFT_WHITE);
    else            tft.drawCircle(x, y, 5, TFT_WHITE);
  }
}

void ui_scan_tick(int tracks_count)
{
  uint32_t now = millis();
  if (now - s_scan_last_ms < 150) return;
  s_scan_last_ms = now;

  s_scan_phase = (s_scan_phase + 1) % 3;

  ui_lock();
  draw_scan_dots(s_scan_phase);

  // 更新曲目计数
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.fillRect(0, 185, 240, 30, TFT_BLACK);
  draw_center_text(String("已扫描 " + String(tracks_count) + " 首歌曲").c_str(), 190);
  ui_unlock();
}

void ui_scan_end()
{
  // 无操作
}

void ui_clear_screen()
{
  ui_lock();
  // 清除屏幕
  tft.fillScreen(TFT_BLACK);
  ui_unlock();
}

// ===== 播放器视图切换 =====
enum ui_player_view_t ui_get_view() { return s_view; }

static inline void ui_set_view(ui_player_view_t new_view)
{
  const uint32_t now_ms = millis();
  ui_lock();
  s_view = new_view;
  s_rot_last_ms = now_ms;   // 防 dt 累积跳角度
  ui_unlock();
}

void ui_toggle_view()
{
  ui_set_view((s_view == UI_VIEW_ROTATE) ? UI_VIEW_INFO : UI_VIEW_ROTATE);
}

void ui_set_now_playing(const char* title, const char* artist)
{
  ui_lock();
  s_np_title  = title  ? String(title)  : String("");
  s_np_artist = artist ? String(artist) : String("");
  ui_unlock();
}

void ui_set_album(const String& album)
{
  ui_lock();
  s_np_album = album;
  ui_unlock();
}

void ui_set_volume(uint8_t vol)
{
  if (vol > 100) vol = 100;
  s_ui_volume = vol;
}

void ui_volume_key_pressed()
{
  s_ui_volume_active_time = millis();  // 更新音量激活时间
}

void ui_set_play_mode(play_mode_t mode)
{
  s_ui_play_mode = mode;
  s_ui_mode_switch_time = millis();  // 记录模式切换时间，用于高亮显示
}

void ui_set_track_pos(int idx, int total)
{
  if (total < 0) total = 0;
  if (idx < 0) idx = 0;
  s_ui_track_idx = idx;
  s_ui_track_total = total;
}