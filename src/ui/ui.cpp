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
#include "lyrics/lyrics.h"
#include "player_state.h"

lgfx::U8g2font g_font_cjk(u8g2_font_wenquanyi_merged);

// UTF-8 字符长度计算
static int utf8_char_len(uint8_t c)
{
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0) return 2;
  if ((c & 0xF0) == 0xE0) return 3;
  if ((c & 0xF8) == 0xF0) return 4;
  return 1;
}

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
static constexpr uint32_t UI_FPS_INFO   = 20; // 信息视图（提升帧率使歌词滚动更丝滑）
static constexpr uint32_t UI_FPS_OTHER  = 1;  // 其它界面

static volatile enum ui_player_view_t s_view = UI_VIEW_INFO;
static String s_np_title;
static String s_np_artist;
String s_np_album;

// ===== 滚动文本状态（像素偏移）=====
static int s_title_scroll_x = 0;      // 标题滚动像素偏移
static int s_artist_scroll_x = 0;     // 歌手滚动像素偏移
static int s_album_scroll_x = 0;      // 专辑滚动像素偏移
static uint32_t s_scroll_last_ms = 0; // 上次滚动时间
static constexpr int SCROLL_SPEED = 1;    // 滚动速度（像素/帧）
static constexpr int SCROLL_GAP = 20;     // 滚动副本间距（像素）

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

static LGFX_Sprite s_coverSpr(&tft);      // 原始封面（无遮罩）→ 供 ROTATE 旋转
static LGFX_Sprite s_coverMasked(&tft);   // 带遮罩封面 → 供 INFO 显示
static bool s_coverSprInited = false;
static bool s_coverSprReady  = false;

static LGFX_Sprite s_frame0(&tft);
static LGFX_Sprite s_frame1(&tft);
static LGFX_Sprite* s_frame[2] = { &s_frame0, &s_frame1 };
static uint8_t s_front = 0;
static uint8_t s_back  = 1;
static bool s_framesInited = false;

// 旋转视图双缓冲帧
static LGFX_Sprite s_rotFrame0(&tft);
static LGFX_Sprite s_rotFrame1(&tft);
static LGFX_Sprite* s_rotFrame[2] = { &s_rotFrame0, &s_rotFrame1 };
static uint8_t s_rotFront = 0;
static uint8_t s_rotBack  = 1;
static bool s_rotFramesInited = false;

static LGFX_Sprite* s_src = nullptr;  // 指向 s_coverSpr，用于旋转

// 列表选择模式缓存
static int s_list_last_drawn_idx = -1;  // 上次绘制的列表选择索引

static float s_angle_deg = 0.0f;
static uint32_t s_rot_last_ms = 0;

// ---------------- 扫描 UI 状态 ----------------
static uint32_t s_scan_last_ms = 0;
static int s_scan_phase = 0;

static volatile bool s_ui_hold = false;

void ui_hold_render(bool hold)
{
  s_ui_hold = hold;
  if (hold) {
    // 进入 hold 时刷新 rot 时钟，避免解除 hold 后角度跳变
    s_rot_last_ms = millis();
  } else {
    // 解除 hold 时，立即通知 UI 任务唤醒，避免等待下一个周期
    s_rot_last_ms = millis(); // 重置旋转时间戳，避免角度跳变
    if (s_ui_task) {
      xTaskNotifyGive(s_ui_task);
    }
  }
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
    ui_lock(); // 必须加锁，因为 UiTask 可能正在读取 s_coverSpr
    s_coverSpr.fillScreen(TFT_BLACK);
    bool ok = false;
    if (is_png) ok = s_coverSpr.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    else        ok = s_coverSpr.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    if (ok) {
      // 复制到遮罩精灵并添加遮罩
      s_coverMasked.fillScreen(TFT_BLACK);
      s_coverSpr.pushSprite(&s_coverMasked, 0, 0);
      s_coverMasked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
    }
    ui_unlock();
    return ok;
  }

  // 安全阈值：防止超大封面炸内存
  if (srcW > 800 || srcH > 800) {
    ui_lock(); // 必须加锁，因为 UiTask 可能正在读取 s_coverSpr
    s_coverSpr.fillScreen(TFT_BLACK);
    bool ok = is_png ? s_coverSpr.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE)
                     : s_coverSpr.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    if (ok) {
      // 复制到遮罩精灵并添加遮罩
      s_coverMasked.fillScreen(TFT_BLACK);
      s_coverSpr.pushSprite(&s_coverMasked, 0, 0);
      s_coverMasked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
    }
    ui_unlock();
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
    ui_lock(); // 必须加锁，因为 UiTask 可能正在读取 s_coverSpr
    s_coverSpr.fillScreen(TFT_BLACK);
    bool ok = is_png ? s_coverSpr.drawPng(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE)
                    : s_coverSpr.drawJpg(ptr, len, 0, 0, COVER_SIZE, COVER_SIZE);
    if (ok) {
      // 复制到遮罩精灵并添加遮罩
      s_coverMasked.fillScreen(TFT_BLACK);
      s_coverSpr.pushSprite(&s_coverMasked, 0, 0);
      s_coverMasked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);
    }
    ui_unlock();
    return ok;
  }

  tmp.fillScreen(TFT_BLACK);

  bool ok = false;
  if (is_png) ok = tmp.drawPng(ptr, len, 0, 0);
  else        ok = tmp.drawJpg(ptr, len, 0, 0);

  if (!ok) {
    tmp.deleteSprite();
    return false;
  }

  s_coverSpr.fillScreen(TFT_BLACK);
  cover_downscale_bilinear(tmp, s_coverSpr, COVER_SIZE, true);

  s_coverMasked.fillScreen(TFT_BLACK);
  s_coverSpr.pushSprite(&s_coverMasked, 0, 0);
  s_coverMasked.fillRectAlpha(0, 0, COVER_SIZE, COVER_SIZE, 128, TFT_BLACK);

  tmp.deleteSprite();
  return true;
}

// 前向声明
static void cover_sprite_init_once();

static void cover_draw_placeholder(const char* msg)
{
  cover_sprite_init_once();
  s_coverSpr.fillScreen(TFT_BLACK);
  s_coverSpr.drawRect(0, 0, COVER_SIZE, COVER_SIZE, 0x7BEF);  // 亮灰边框

  s_coverSpr.setTextSize(2);
  s_coverSpr.setTextColor(TFT_WHITE);

  const char* s = (msg && msg[0]) ? msg : "NO COVER";
  int16_t x = (COVER_SIZE - s_coverSpr.textWidth(s)) / 2;
  if (x < 0) x = 0;
  s_coverSpr.setCursor(x, 110);
  s_coverSpr.print(s);
}

// 初始化封面精灵（仅执行一次）
// 功能: 创建 s_coverSpr（原始）和 s_coverMasked（带遮罩）两个精灵
//      如果检测到 PSRAM，则使用 PSRAM 以节省内部 RAM
static void cover_sprite_init_once()
{
  // 如果已经初始化过，直接返回
  if (s_coverSprInited) return;

  // 检测 PSRAM 并配置精灵
  const bool has_psram = psramFound();

  // 原始封面精灵（供 ROTATE 视图旋转）
  s_coverSpr.setColorDepth(16);
  s_coverSpr.setPsram(has_psram);
  if (!s_coverSpr.createSprite(COVER_SIZE, COVER_SIZE)) {
    LOGW("[COVER] s_coverSpr.createSprite(%d,%d) failed", COVER_SIZE, COVER_SIZE);
    return;
  }
  s_coverSpr.fillScreen(TFT_BLACK);

  // 带遮罩封面精灵（供 INFO 视图显示）
  s_coverMasked.setColorDepth(16);
  s_coverMasked.setPsram(has_psram);
  if (!s_coverMasked.createSprite(COVER_SIZE, COVER_SIZE)) {
    LOGW("[COVER] s_coverMasked.createSprite(%d,%d) failed", COVER_SIZE, COVER_SIZE);
    s_coverSpr.deleteSprite();
    return;
  }
  s_coverMasked.fillScreen(TFT_BLACK);

  s_coverSprInited = true;
}

// 初始化双缓冲帧精灵（仅执行一次）
// 功能: 创建 INFO 视图和 ROTATE 视图的双缓冲帧
//      如果检测到 PSRAM，则使用 PSRAM 以节省内部 RAM
static void cover_frames_init_once()
{
  // 如果已经初始化过，直接返回
  if (s_framesInited) return;

  const bool has_psram = psramFound();

  // INFO 视图双缓冲帧
  for (int i = 0; i < 2; ++i) {
    s_frame[i]->setColorDepth(16);
    s_frame[i]->setPsram(has_psram);
    if (!s_frame[i]->createSprite(COVER_SIZE, COVER_SIZE)) {
      LOGW("[COVER] s_frame[%d].createSprite(%d,%d) failed", i, COVER_SIZE, COVER_SIZE);
      // 清理已创建的精灵
      for (int j = 0; j < i; ++j) {
        s_frame[j]->deleteSprite();
      }
      return;
    }
    s_frame[i]->fillScreen(TFT_BLACK);
  }
  s_front = 0;
  s_back  = 1;
  s_framesInited = true;

  // ROTATE 视图双缓冲帧
  for (int i = 0; i < 2; ++i) {
    s_rotFrame[i]->setColorDepth(16);
    s_rotFrame[i]->setPsram(has_psram);
    if (!s_rotFrame[i]->createSprite(COVER_SIZE, COVER_SIZE)) {
      LOGW("[COVER] s_rotFrame[%d].createSprite(%d,%d) failed", i, COVER_SIZE, COVER_SIZE);
      // 清理已创建的精灵
      for (int j = 0; j < i; ++j) {
        s_rotFrame[j]->deleteSprite();
      }
      return;
    }
    s_rotFrame[i]->fillScreen(TFT_BLACK);
  }
  s_rotFront = 0;
  s_rotBack  = 1;
  s_rotFramesInited = true;
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
  // 检查旋转帧是否已初始化以及源精灵是否有效
  if (!s_rotFramesInited || !s_src) return;

  // 获取后帧指针（旋转专用双缓冲）
  auto* dst = s_rotFrame[s_rotBack];
  // 清空后帧
  dst->fillScreen(TFT_BLACK);
  // 将源精灵旋转指定角度并绘制到后帧（不缩放）
  s_src->pushRotateZoom(dst, COVER_SIZE / 2, COVER_SIZE / 2, angle_deg, 1.0f, 1.0f);

  // 将后帧推送到屏幕 (0, 0) 位置
  dst->pushSprite(0, 0);

  // 交换前后帧索引（双缓冲）
  uint8_t tmp = s_rotFront;
  s_rotFront = s_rotBack;
  s_rotBack = tmp;
}

/**
 * 基于时间的推移式滚动
 * @param progress: 当前句播放进度 (0.0 到 1.0)
 */
static void draw_scrolling_line_by_time(LGFX_Sprite* dst, const char* text, int y, 
                                       int safe_pad, uint16_t color, float progress)
{
  if (!text || strlen(text) == 0) return;

  dst->setTextColor(color);
  dst->setFont(&g_font_cjk);
  int text_w = dst->textWidth(text);
  int available_w = 240 - (safe_pad * 2);

  if (text_w <= available_w) {
    draw_center_text_on_sprite(dst, text, y, color, safe_pad);
    return;
  }

  // 计算最大可滚动距离
  int max_scroll = text_w - available_w;

  // --- 核心逻辑：进度映射 ---
  // 我们让进度在 10% 到 90% 的时间内进行滚动，前后留出时间让用户看清头尾
  float scroll_factor = 0;
  if (progress < 0.1f) {
    scroll_factor = 0; // 头部静止
  } else if (progress > 0.9f) {
    scroll_factor = 1.0f; // 尾部静止（刚好贴在右侧）
  } else {
    // 中间 80% 的时间用来平滑移动
    scroll_factor = (progress - 0.1f) / 0.8f;
  }

  int current_offset = (int)(max_scroll * scroll_factor);

  // 裁剪并绘制
  // 裁剪区域高度设置为 26，确保完整显示当前行文字（包含字体的上伸/下伸部分）
  dst->setClipRect(safe_pad, y - 11, available_w, 26);
  dst->setCursor(safe_pad - current_offset, y);
  dst->print(text);
  dst->clearClipRect();
}

static void cover_info_draw()
{
  if (!s_framesInited) return;

  uint32_t t0 = millis();

  auto* dst = s_frame[s_back];
  dst->fillScreen(TFT_BLACK);

  uint32_t t1 = millis();

  // 1) 静态封面（整屏）- 使用带遮罩的版本
  if (s_coverSprReady) {
    s_coverMasked.pushSprite(dst, 0, 0);
  }
  uint32_t t_cover = millis();

  // 2) 参数：圆屏安全边距 + 更大的字号 + 更紧凑的排布
  const int safe_pad = 12;

  const uint16_t c_title  = UI_COLOR_TITLE;   // 歌名文字颜色（纯白）
  const uint16_t c_artist = UI_COLOR_ARTIST;  // 歌手文字颜色（浅灰）
  const uint16_t c_lyrics = 0xFFFF;           // 歌词颜色（白色）
  const uint16_t c_lyrics_next = 0xAD55;      // 下一句歌词颜色（亮灰）

  // 3) 把信息区抬高一点，避免圆屏底部变窄导致左右被裁

  const int y_status = 131;  // 状态栏（音量/模式/列表）上移1像素
  const int y_bar   = 149;   // 进度条下移1像素
  const int y_time  = 157;   // 时间（上移3像素）
  const int y_title = 176;   // 标题
  const int y_artist= 195;   // 歌手（下移3像素）

  // 4) 歌词显示（屏幕上半部分）- 在遮罩之后绘制，确保可见
  bool hasLyrics = g_lyricsDisplay.hasLyrics();
  LOGD("[UI] hasLyrics: %d", hasLyrics);
  if (hasLyrics) {
    // 使用滚动歌词显示（3行：上一句、当前、下一句）
    LyricsDisplay::ScrollLyrics scroll = g_lyricsDisplay.getScrollLyrics();
    
    // 歌词显示位置 - 使用 constexpr 便于编译器优化
    static constexpr int Y_LYRICS_CENTER = 93;   // 中心位置
    static constexpr int LINE_HEIGHT = 20;       // 行高
    static constexpr int ANIM_END_PERCENT = 80;  // 前80%完成动画
    
    // 预计算缓动表（0-100 映射到 0-100 的 ease-out 曲线）
    // ease-out: y = 1 - (1-x)^2，展开后避免 pow 调用
    static const uint8_t ease_table[101] = {
      0,  2,  4,  6,  8, 10, 12, 14, 15, 17, 19, 21, 23, 24, 26, 28,
     30, 31, 33, 35, 36, 38, 40, 41, 43, 44, 46, 47, 49, 50, 52, 53,
     55, 56, 58, 59, 60, 62, 63, 64, 66, 67, 68, 70, 71, 72, 73, 75,
     76, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91,
     91, 92, 93, 94, 94, 95, 96, 96, 97, 97, 98, 98, 99, 99, 99,100,
    100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,100,
    100,100,100,100,100
    };
    
    // 将进度转换为 0-100 的整数索引
    int progress_int = (int)(scroll.progress * 100.0f);
    if (progress_int > 100) progress_int = 100;
    if (progress_int < 0) progress_int = 0;
    
    // 计算动画进度（前80%完成动画）
    int anim_progress_int;
    if (progress_int < ANIM_END_PERCENT) {
      anim_progress_int = (progress_int * 100) / ANIM_END_PERCENT;
    } else {
      anim_progress_int = 100;
    }
    // 安全检查：确保不越界
    if (anim_progress_int > 100) anim_progress_int = 100;
    if (anim_progress_int < 0) anim_progress_int = 0;

    // 查表获取缓动值，计算偏移（避免浮点运算）
    int offset = (ease_table[anim_progress_int] * LINE_HEIGHT) / 100;
    
    // 绘制上一句（灰色，淡出效果）
    if (scroll.prev.length() > 0) {
      draw_center_text_on_sprite(dst, scroll.prev.c_str(), 
                                Y_LYRICS_CENTER - LINE_HEIGHT - offset, 
                                c_lyrics_next, safe_pad);
    }
    
    // 绘制当前句（白色，高亮，支持基于时间的滚动）
    if (scroll.current.length() > 0) {
      draw_scrolling_line_by_time(dst, scroll.current.c_str(), 
                                Y_LYRICS_CENTER - offset, 
                                safe_pad, c_lyrics, scroll.progress);
    }
    
    // 绘制下一句（灰色，淡入效果）
    if (scroll.next.length() > 0) {
      draw_center_text_on_sprite(dst, scroll.next.c_str(), 
                                Y_LYRICS_CENTER + LINE_HEIGHT - offset, 
                                c_lyrics_next, safe_pad);
    }
  }

  uint32_t t_lyrics = millis();

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

  uint32_t t_status = millis();

  // 5) 标题/歌手（支持滚动显示长文本）
  extern void draw_note_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
  extern void draw_artist_icon_img(LGFX_Sprite* dst, int x, int y, uint16_t color);
  
  // 更新滚动偏移（像素滚动，30ms间隔）
  uint32_t now = millis();
  if (now - s_scroll_last_ms > 30) {
    s_scroll_last_ms = now;
    
    // 标题滚动
    bool title_scroll = draw_scrolling_text_with_icon(dst, y_title, s_np_title, s_title_scroll_x, 
                                                       14, c_title, safe_pad, draw_note_icon_img);
    if (title_scroll) {
      s_title_scroll_x += SCROLL_SPEED;
      // 滚动范围：文本宽度 + 间距
      extern lgfx::U8g2font g_font_cjk;
      dst->setFont(&g_font_cjk);
      int title_w = dst->textWidth(s_np_title.c_str());
      if (s_title_scroll_x > title_w + SCROLL_GAP) {
        s_title_scroll_x = 0;
      }
    } else {
      s_title_scroll_x = 0;
    }
    
    // 歌手滚动
    bool artist_scroll = draw_scrolling_text_with_icon(dst, y_artist, s_np_artist, s_artist_scroll_x,
                                                        14, c_artist, safe_pad, draw_artist_icon_img);
    if (artist_scroll) {
      s_artist_scroll_x += SCROLL_SPEED;
      extern lgfx::U8g2font g_font_cjk;
      dst->setFont(&g_font_cjk);
      int artist_w = dst->textWidth(s_np_artist.c_str());
      if (s_artist_scroll_x > artist_w + SCROLL_GAP) {
        s_artist_scroll_x = 0;
      }
    } else {
      s_artist_scroll_x = 0;
    }
  } else {
    // 使用当前偏移绘制
    draw_scrolling_text_with_icon(dst, y_title, s_np_title, s_title_scroll_x, 
                                  14, c_title, safe_pad, draw_note_icon_img);
    draw_scrolling_text_with_icon(dst, y_artist, s_np_artist, s_artist_scroll_x,
                                  14, c_artist, safe_pad, draw_artist_icon_img);
  }

  uint32_t t_text = millis();

  // 6) 推屏
  dst->pushSprite(0, 0);

  uint32_t t_push = millis();

  uint8_t tmp = s_front;
  s_front = s_back;
  s_back  = tmp;
}

static inline TickType_t ui_period_ticks()
{
  // hold 期间：不画，但要"醒得勤快一点"，保证解除 hold 后立刻恢复（这里按旋转帧率）
  if (s_ui_hold) return pdMS_TO_TICKS(1000 / UI_FPS_ROTATE);

  // 列表选择模式：使用较高帧率以实现平滑滚动
  if (player_is_in_list_select_mode()) return pdMS_TO_TICKS(1000 / 20);

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
  for (;;) {
    const uint32_t now_ms = millis();

    // 动态帧率：rotate 20fps / info 20fps / other 1fps
    TickType_t period = ui_period_ticks();
    if (period == 0) period = 1;

    // 使用 ulTaskNotifyTake 实现可中断的延迟
    // 正常情况下等待 period 时长，但收到通知时立即唤醒
    ulTaskNotifyTake(pdTRUE, period);

    // hold：不渲染，但刷新 rot 时钟，避免解除 hold 后 dt 巨大导致角度跳变
    if (s_ui_hold) {
      s_rot_last_ms = millis();
      continue;
    }

    // 检查是否处于列表选择模式
    if (player_is_in_list_select_mode()) {
      // 列表选择模式下持续重绘（用于滚动显示）
      ui_lock();
      int current_idx = player_get_list_selected_idx();
      ListSelectState state = player_get_list_select_state();
      const char* title = (state == ListSelectState::ARTIST) ? "选择歌手" : "选择专辑";
      const auto& groups = player_get_list_groups();
      ui_draw_list_select(groups, current_idx, title);
      s_list_last_drawn_idx = current_idx;
      ui_unlock();
      continue;
    }

    // 只在 PLAYER 界面、封面就绪时推屏
    if (s_screen == UI_SCREEN_PLAYER && s_coverSprReady && s_framesInited) {
      ui_lock();

      // 更新歌词时间（在绘制前更新）
      uint32_t play_ms = audio_get_play_ms();
      g_lyricsDisplay.updateTime(play_ms);

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
  uint32_t t0 = millis();
  
  // 初始化封面精灵
  cover_sprite_init_once();
  uint32_t t1 = millis();

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

  uint32_t t2 = millis();
  
  // 使用 MIME 类型判断图像格式（比猜测更可靠）
  if (t.cover_mime == "image/png") is_png = true;
  else if (t.cover_mime == "image/jpeg") is_png = false;

  // 将封面图像缩放到 240x240
  ui_lock();
  uint32_t t3 = millis();
  bool ok = cover_blit_scaled_to_240(ptr, len, is_png);
  uint32_t t4 = millis();

  s_coverSprReady = ok;
  LOGI("[COVER] decode: init=%lums, load=%lums, scale=%lums, total=%lums", 
       t1-t0, t2-t1, t4-t3, t4-t0);
  ui_unlock();
  return ok;
}

// 封面数据缓存（用于拆分加载）
static const uint8_t* s_cover_mem_ptr = nullptr;
static size_t s_cover_mem_len = 0;
static bool s_cover_mem_is_png = false;

// 从 SD 读取封面到内存（SD 卡操作）
bool ui_cover_load_to_memory(const TrackInfo& t)
{
  s_cover_mem_ptr = nullptr;
  s_cover_mem_len = 0;
  s_cover_mem_is_png = false;

  cover_sprite_init_once();

  const uint8_t* ptr = nullptr;
  size_t len = 0;
  bool is_png = false;

  if (!cover_load_to_memory(sd, t, ptr, len, is_png)) {
    return false;
  }

  s_cover_mem_ptr = ptr;
  s_cover_mem_len = len;
  s_cover_mem_is_png = is_png;
  return true;
}

// 从内存解码缩放到精灵（不访问 SD）
bool ui_cover_scale_from_memory()
{
  if (!s_cover_mem_ptr || s_cover_mem_len == 0) {
    return false;
  }

  ui_lock();
  bool ok = cover_blit_scaled_to_240(s_cover_mem_ptr, s_cover_mem_len, s_cover_mem_is_png);
  s_coverSprReady = ok;
  if (ok) {
    s_src = &s_coverSpr;  // 设置旋转源精灵
    s_angle_deg = 0.0f;  // 重置旋转角度
    s_rot_last_ms = 0;   // 重置旋转时间戳
  }
  ui_unlock();

  s_cover_mem_ptr = nullptr;
  s_cover_mem_len = 0;
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
  
  // 确保 UI 任务创建完成后再开启渲染开关
  s_ui_hold = false;

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
  
  // 更新歌词显示时间
  g_lyricsDisplay.updateTime(play_ms);
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

void ui_scan_abort()
{
  ui_lock();
  
  // 显示"已取消"提示
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  
  tft.setTextSize(2);
  draw_center_text("已取消", 100);
  
  tft.setTextSize(1);
  draw_center_text("扫描已中断", 140);
  
  // 延迟一段时间让用户看到提示
  delay(1500);
  
  ui_unlock();
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

// 设置播放器视图（带线程锁保护）
// 参数: new_view - 新视图类型（ROTATE旋转封面 或 INFO信息详情）
// 功能: 更新视图状态，重置旋转时间戳防止角度跳变
static inline void ui_set_view(ui_player_view_t new_view)
{
  const uint32_t now_ms = millis();
  ui_lock();
  s_view = new_view;
  s_rot_last_ms = now_ms;   // 防 dt 累积跳角度
  ui_unlock();
}

// 切换播放器视图（长按PLAY键触发）
// 在旋转视图(ROTATE)和信息视图(INFO)之间切换
void ui_toggle_view()
{
  LOGI("[UI] toggle_view: current=%d", (int)s_view);
  ui_set_view((s_view == UI_VIEW_ROTATE) ? UI_VIEW_INFO : UI_VIEW_ROTATE);
  LOGI("[UI] toggle_view: new=%d", (int)s_view);
}

void ui_set_now_playing(const char* title, const char* artist)
{
  ui_lock();
  s_np_title  = title  ? String(title)  : String("");
  s_np_artist = artist ? String(artist) : String("");
  // 切歌时重置滚动偏移
  s_title_scroll_x = 0;
  s_artist_scroll_x = 0;
  s_scroll_last_ms = 0;
  ui_unlock();
}

void ui_set_album(const String& album)
{
  ui_lock();
  s_np_album = album;
  ui_unlock();
  // 切歌时重置专辑滚动偏移
  reset_album_scroll();
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
}

void ui_mode_switch_highlight()
{
  s_ui_mode_switch_time = millis();  // 记录模式切换时间，用于高亮显示
}

void ui_set_track_pos(int idx, int total)
{
  if (total < 0) total = 0;
  if (idx < 0) idx = 0;
  s_ui_track_idx = idx;
  s_ui_track_total = total;
}

// =============================================================================
// 列表选择界面绘制
// =============================================================================
// 字符串处理函数

// 按像素宽度截断字符串，支持UTF-8中文字符
// 参数: text - 原始字符串
//       maxWidth - 最大像素宽度
// 返回: 截断后的字符串（不含省略号）
static String truncateByPixel(const String& text, int maxWidth)
{
  String result = text;
  while (result.length() > 0) {
    // 先检查当前字符串是否符合宽度要求
    if (tft.textWidth(result) <= maxWidth) {
      break;
    }
    
    // 按字节长度回退到上一个完整的字符起始位
    int len = result.length();
    // 从后向前找到第一个字符的起始位置
    while (len > 0) {
      unsigned char c = result.charAt(len - 1);
      // UTF-8 字符起始字节的最高两位不是 10
      if ((c & 0xC0) != 0x80) {
        break;
      }
      len--;
    }
    // 如果找到起始位置，截取到该位置
    if (len > 0) {
      result = result.substring(0, len - 1);
    } else {
      // 没有找到有效字符，清空
      result = "";
    }
  }
  return result;
}

// 按字符偏移截取子串，支持UTF-8中文字符
// 参数: text - 原始字符串
//       charOffset - 字符偏移量（不是字节偏移）
// 返回: 从偏移位置开始的子串
static String getSubStrByCharOffset(const String& text, int charOffset)
{
  if (charOffset <= 0) return text;
  
  int char_count = 0;
  int byte_pos = 0;
  while (char_count < charOffset && byte_pos < text.length()) {
    unsigned char c = text.charAt(byte_pos);
    if ((c & 0x80) == 0) {
      byte_pos++;  // ASCII字符，1字节
    } else if ((c & 0xE0) == 0xC0) {
      byte_pos += 2;  // 2字节UTF-8字符
    } else if ((c & 0xF0) == 0xE0) {
      byte_pos += 3;  // 3字节UTF-8字符（中文）
    } else if ((c & 0xF8) == 0xF0) {
      byte_pos += 4;  // 4字节UTF-8字符
    } else {
      byte_pos++;
    }
    char_count++;
  }
  
  if (byte_pos < text.length()) {
    return text.substring(byte_pos);
  }
  return "";
}

// 生成滚动文本（支持副本衔接）
static String getScrollingText(const String& text, int pixelOffset, int maxWidth)
{
  // 这里增加 6 个空格作为衔接感，视觉上更自然
  String gap = "      ";
  String extendedText = text + gap + text;

  // 从偏移量位置开始截取（基于像素）
  // 注意：这里我们仍然需要找到合适的字符边界，避免截断在字符中间
  int byte_pos = 0;
  int current_pixel = 0;
  
  while (byte_pos < extendedText.length()) {
    // 计算当前字符的宽度
    int char_len = 0;
    unsigned char c = extendedText.charAt(byte_pos);
    if ((c & 0x80) == 0) char_len = 1;
    else if ((c & 0xE0) == 0xC0) char_len = 2;
    else if ((c & 0xF0) == 0xE0) char_len = 3;
    else char_len = 4;
    
    String char_str = extendedText.substring(byte_pos, byte_pos + char_len);
    int char_width = tft.textWidth(char_str);
    
    if (current_pixel + char_width > pixelOffset) {
      break;
    }
    
    current_pixel += char_width;
    byte_pos += char_len;
  }
  
  String startedText = extendedText.substring(byte_pos);

  // 关键：必须按像素宽度截断，确保它不会覆盖到后面的"（多少首）"
  return truncateByPixel(startedText, maxWidth);
}

// =============================================================================
// 滚动控制结构体
struct ListScrollState {
  int scroll_idx = -1;          // 当前滚动项索引
  int scroll_offset = 0;        // 当前滚动偏移（像素）
  uint32_t last_scroll_time = 0; // 上次滚动时间
  static constexpr uint32_t SCROLL_INTERVAL_MS = 50; // 加快滚动频率以获得更丝滑的效果
  static constexpr int SCROLL_STEP_PX = 2; // 每次滚动的像素数
  
  // 重置滚动状态
  void reset(int new_idx) {
    scroll_idx = new_idx;
    scroll_offset = 0;
    last_scroll_time = millis();
  }
  
  // 检查是否需要更新滚动（时间间隔到达）
  bool shouldUpdate() {
    uint32_t now = millis();
    if (now - last_scroll_time >= SCROLL_INTERVAL_MS) {
      last_scroll_time = now;
      return true;
    }
    return false;
  }
  
  // 更新滚动偏移
  // 参数: name - 原始字符串
  //       full_text_width - 完整文本宽度
  //       available_width - 可用宽度
  // 返回: 偏移是否有变化，需要重绘
  bool update(const String& name, int full_text_width, int available_width) {
    if (full_text_width <= available_width) {
      // 文本无需滚动，保持偏移0
      scroll_offset = 0;
      return false;
    }

    scroll_offset += SCROLL_STEP_PX;
    
    // 计算总滚动长度（包含间隙）
    String gap = "      ";
    int gap_width = tft.textWidth(gap);
    int total_scroll_width = full_text_width + gap_width;
    
    // 滚动到末尾后重置
    if (scroll_offset >= total_scroll_width) {
      scroll_offset = 0;
    }
    return true;
  }
};

// 列表选择界面静态变量
static int s_last_selected_idx = -1;
static int s_last_start_idx = -1;
static bool s_first_draw = true;
static int last_drawn_selected = -1;
static int last_drawn_offset = -1;
static ListScrollState s_scroll_state; // 滚动状态

// =============================================================================
// 绘制辅助函数

// 绘制列表框架（标题、分隔线、滚动条、底部提示）
// 参数: title - 标题文字
//       start_idx - 当前显示起始索引
//       total - 总项目数
//       visible - 可见项目数
static void drawListFrame(const char* title, int start_idx, int total, int visible)
{
  // 绘制标题
  tft.setTextSize(1);
  tft.setTextColor(TFT_WHITE);
  draw_center_text(title, 25);

  // 绘制分隔线
  tft.drawFastHLine(20, 40, 200, TFT_WHITE);

  // 绘制滚动指示器
  if (total > visible) {
    const int ITEM_HEIGHT = 18;
    const int START_Y = 60;
    int bar_height = visible * ITEM_HEIGHT;
    int bar_y = START_Y - 7;
    int thumb_height = (visible * bar_height) / total;
    if (thumb_height < 10) thumb_height = 10;

    int thumb_y = bar_y + (start_idx * (bar_height - thumb_height)) / (total - visible);
    
    // 确保滑块不超出滚动条范围
    if (thumb_y < bar_y) thumb_y = bar_y;
    if (thumb_y + thumb_height > bar_y + bar_height) thumb_y = bar_y + bar_height - thumb_height;

    tft.drawRect(225, bar_y, 4, bar_height, TFT_DARKGREY);
    tft.fillRect(225, thumb_y, 4, thumb_height, TFT_WHITE);
  }

  // 绘制底部提示（分两行，居中显示）
  tft.setTextColor(TFT_LIGHTGREY);
  draw_center_text("NEXT/PREV:选择 VOL:翻页", 171);
  draw_center_text("PLAY:确认 MODE:取消", 185);
}

// 绘制单个列表项
// 参数: group - 列表组数据
//       idx - 项目索引
//       list_pos - 列表中的显示位置（0-4）
//       is_selected - 是否选中
//       scroll_offset - 滚动偏移（仅选中项有效）
//       draw_bg - 是否绘制背景（默认true，滚动更新时可设为false避免重绘背景）
static void drawListItem(const PlaylistGroup& group, int idx, int list_pos, 
                         bool is_selected, int scroll_offset = 0, bool draw_bg = true)
{
  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  int y = START_Y + list_pos * ITEM_HEIGHT;
  
  // 绘制选中背景（仅在需要时绘制）
  if (is_selected && draw_bg) {
    tft.fillRoundRect(15, y - ITEM_HEIGHT / 2, 200, ITEM_HEIGHT, 5, 0x4208);
  }

  // 设置文本颜色
  tft.setTextColor(is_selected ? TFT_YELLOW : TFT_WHITE);

  String name = group.name;
  
  // 计算序号宽度
  String prefix = String(idx + 1) + ". ";
  int prefix_width = tft.textWidth(prefix);
  
  // 计算歌曲数量宽度
  String count = "(" + String(group.track_indices.size()) + "首)";
  int count_width = tft.textWidth(count);
  
  // 重点：计算中间的可显示区域
  int list_left_edge = 25;           // 列表文字起始 X
  int list_right_edge = 210;         // 列表文字结束 X (留点边距)
  
  int text_start_x = list_left_edge + prefix_width;
  int text_end_x = list_right_edge - count_width;
  int available_width = text_end_x - text_start_x;
  
  // 检查是否需要滚动
  int name_width = tft.textWidth(name);
  bool need_scroll = is_selected && (name_width > available_width);
  
  if (need_scroll) {
    // 使用新的副本衔接函数
    // 注意：这里不再需要判断 scroll_offset > 0，因为 0 也是合法的起始位置
    String scroll_name = getScrollingText(name, scroll_offset, available_width);
    
    // 1. 正常绘制序号
    tft.setCursor(list_left_edge, y - 7);
    tft.print(prefix);
    
    // 2. 设置裁剪区域：只允许在 [text_start_x] 到 [text_end_x] 之间显示
    tft.setClipRect(text_start_x, y - 10, available_width, 20);
    
    tft.setCursor(text_start_x, y - 7);
    tft.print(scroll_name);
    
    // 3. 立即清除裁剪设置，否则后面所有的绘制都看不见了
    tft.clearClipRect();

    // 4. 绘制数量（它会在 text_end_x 之后，不会被滚动文字覆盖）
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setCursor(list_right_edge - count_width, y - 7);
    tft.print(count);
  } else {
    // 非滚动状态：截断显示
    String display_name = name;
    
    if (name_width > available_width) {
      // 需要截断
      display_name = truncateByPixel(name, available_width);
      // 添加"…"占位符
      if (display_name.length() < name.length()) {
        int ellipsis_width = tft.textWidth("...");
        if (tft.textWidth(display_name) + ellipsis_width <= available_width) {
          display_name += "...";
        } else {
          display_name = truncateByPixel(display_name, available_width - ellipsis_width);
          display_name += "...";
        }
      }
    }
    
    // 绘制序号和名字
    tft.setCursor(list_left_edge, y - 7);
    tft.print(prefix + display_name);
    
    // 绘制数量
    tft.setTextColor(TFT_LIGHTGREY);
    tft.setCursor(text_end_x, y - 7);
    tft.print(count);
  }
}

// =============================================================================
// 绘制列表选择界面
// 参数: groups - 列表组数据
//       selected_idx - 当前选中的索引
//       title - 界面标题（"选择歌手"或"选择专辑"）
// 注意: 调用此函数前必须已获取 ui_lock()
void ui_draw_list_select(const std::vector<PlaylistGroup>& groups, int selected_idx, const char* title)
{
  if (groups.empty()) return;

  // 计算显示范围（最多显示5项）
  const int ITEMS_VISIBLE = 5;
  const int ITEM_HEIGHT = 18;
  const int START_Y = 60;
  const int SCROLL_GAP = 20;

  int total = (int)groups.size();
  int start_idx = 0;

  // 检测是否是新进入列表选择模式（选中项发生大跳变）
  // 使用 ITEMS_VISIBLE 来判断是否是翻页操作
  bool is_jump = (s_last_selected_idx == -1 || abs(selected_idx - s_last_selected_idx) >= ITEMS_VISIBLE);

  if (is_jump) {
    s_first_draw = true;
  }

  // 检查选中项是否改变，改变则重置滚动偏移
  if (selected_idx != s_scroll_state.scroll_idx) {
    s_scroll_state.reset(selected_idx);
  }

  // 【关键修改 1】：在所有绘制开始前，先处理滚动计时和偏移更新
  bool is_offset_changed = false;
  if (selected_idx == s_scroll_state.scroll_idx) {
    if (s_scroll_state.shouldUpdate()) {
      // 计算当前选中项是否真的需要滚动
      String prefix = String(selected_idx + 1) + ". ";
      int prefix_width = tft.textWidth(prefix);
      String count = "(" + String(groups[selected_idx].track_indices.size()) + "首)";
      int count_width = tft.textWidth(count);
      int available_width = 200 - 25 - prefix_width - count_width - 10;
      int full_width = tft.textWidth(groups[selected_idx].name);
      
      // 计算字符数量
      int char_count = 0;
      int byte_pos = 0;
      String name = groups[selected_idx].name;
      while (byte_pos < name.length()) {
        unsigned char c = name.charAt(byte_pos);
        if ((c & 0x80) == 0) {
          byte_pos++;  // ASCII字符，1字节
        } else if ((c & 0xE0) == 0xC0) {
          byte_pos += 2;  // 2字节UTF-8字符
        } else if ((c & 0xF0) == 0xE0) {
          byte_pos += 3;  // 3字节UTF-8字符（中文）
        } else if ((c & 0xF8) == 0xF0) {
          byte_pos += 4;  // 4字节UTF-8字符
        } else {
          byte_pos++;
        }
        char_count++;
      }
      
      // 更新偏移量
      is_offset_changed = s_scroll_state.update(name, full_width, available_width);
    }
  }
  
  // 防止频繁重绘检查（非滚动更新时）
  if (!s_first_draw && selected_idx == last_drawn_selected && 
      s_scroll_state.scroll_offset == last_drawn_offset && !is_offset_changed) {
    // 无变化，直接返回
    return;
  }

  // --- 索引计算核心（硬分页逻辑）---

  // 1. 计算当前选中项所在的页码 (每页 ITEMS_VISIBLE 个)
  int current_page = selected_idx / ITEMS_VISIBLE;

  // 2. 这一页的起始索引就是固定的
  start_idx = current_page * ITEMS_VISIBLE;

  // 3. 这里的 end_idx 逻辑保持不变，它会自动处理最后一页不足 5 个的情况
  int end_idx = start_idx + ITEMS_VISIBLE;
  if (end_idx > total) end_idx = total;

  // 4. 判定是否需要全屏重绘
  // 只要起始索引变了（翻页了），就必须全刷
  bool need_scroll = (start_idx != s_last_start_idx);

  // 【关键修改 2】：判断重绘条件
  if (s_first_draw) {
    // 首次绘制，强制全屏重绘
    tft.fillScreen(TFT_BLACK);

    // 绘制列表框架
    drawListFrame(title, start_idx, total, ITEMS_VISIBLE);

    // 绘制所有列表项
    for (int i = start_idx; i < end_idx; i++) {
      int list_pos = i - start_idx;  // 列表中的位置（0-4）
      bool is_selected = (i == selected_idx);
      
      // 使用辅助函数绘制列表项，选中项使用当前滚动偏移
      drawListItem(groups[i], i, list_pos, is_selected, 
                   is_selected ? s_scroll_state.scroll_offset : 0);
    }
  } else if (need_scroll) {
    // 滚动页面切换，完全重绘
    tft.fillScreen(TFT_BLACK);

    // 绘制列表框架
    drawListFrame(title, start_idx, total, ITEMS_VISIBLE);

    // 绘制所有列表项
    for (int i = start_idx; i < end_idx; i++) {
      int list_pos = i - start_idx;  // 列表中的位置（0-4）
      bool is_selected = (i == selected_idx);
      
      // 使用辅助函数绘制列表项，选中项使用当前滚动偏移
      drawListItem(groups[i], i, list_pos, is_selected, 
                   is_selected ? s_scroll_state.scroll_offset : 0);
    }
  } else if (selected_idx != s_last_selected_idx) {
    // 跨行切换逻辑（清除旧高亮，画新高亮）
    // 只更新变化的两行（旧的选中项和新的选中项）
    if (s_last_selected_idx >= 0 && s_last_selected_idx < total && s_last_selected_idx != selected_idx) {
      int old_list_pos = s_last_selected_idx - start_idx;
      if (s_last_selected_idx >= start_idx && s_last_selected_idx < end_idx) {
        // 清除旧的高亮背景
        int old_y = START_Y + old_list_pos * ITEM_HEIGHT;
        tft.fillRoundRect(15, old_y - ITEM_HEIGHT / 2, 200, ITEM_HEIGHT, 5, TFT_BLACK);
        
        // 重新绘制旧的选中项（非选中状态）
        drawListItem(groups[s_last_selected_idx], s_last_selected_idx, old_list_pos, false, 0, false);
      }
    }

    // 绘制新的高亮行（选中状态）
    int new_list_pos = selected_idx - start_idx;
    drawListItem(groups[selected_idx], selected_idx, new_list_pos, true, 
                 s_scroll_state.scroll_offset, true);
  } else if (is_offset_changed) {
    // 【核心】：仅更新选中项的文字部分实现滚动
    int new_list_pos = selected_idx - start_idx;
    // 直接调用 drawListItem，此时 scroll_offset 已经递增过了
    drawListItem(groups[selected_idx], selected_idx, new_list_pos, true, s_scroll_state.scroll_offset, true);
  }

  // 更新状态
  s_last_selected_idx = selected_idx;
  s_last_start_idx = start_idx;
  last_drawn_selected = selected_idx;
  last_drawn_offset = s_scroll_state.scroll_offset;
  s_first_draw = false;
}

// 清除列表选择界面
void ui_clear_list_select()
{
  ui_lock();
  tft.fillScreen(TFT_BLACK);
  
  // 必须重置这些静态变量，否则下次进入会误以为是"局部更新"
  s_first_draw = true;
  s_last_selected_idx = -1;
  s_last_start_idx = -1;
  last_drawn_selected = -1;
  last_drawn_offset = -1;
  s_scroll_state.reset(-1); // 重置滚动状态
  
  // 清除裁剪区域，避免异常退出时裁剪区域未被清除
  tft.clearClipRect();
  
  ui_unlock();
}