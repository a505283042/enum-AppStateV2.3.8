#include <Arduino.h>
#include "keys/keys.h"
#include "keys/keys_pins.h"

#include "app_flags.h"
#include "storage/storage_music.h"
#include "ui/ui.h"
#include "audio/audio_service.h"

// 你项目里这些函数/接口要能被调用
void player_next_track();
void player_prev_track();
void player_toggle_random();
void player_toggle_play();
void player_volume_step(int delta); // 下面我会给你 player_state.cpp 里加
void player_next_group(); // 长按 NEXT：切换到下一个歌手/专辑组

static inline bool pressed(int level) { return level == LOW; } // 按下接地

struct KeyCtx {
  uint8_t pin;
  int last;
  uint32_t t_down;
  bool long_fired;
  uint32_t t_repeat;
};

static KeyCtx k_mode  { PIN_KEY_MODE,  HIGH, 0, false, 0 };
static KeyCtx k_play  { PIN_KEY_PLAY,  HIGH, 0, false, 0 };
static KeyCtx k_prev  { PIN_KEY_PREV,  HIGH, 0, false, 0 };
static KeyCtx k_next  { PIN_KEY_NEXT,  HIGH, 0, false, 0 };
static KeyCtx k_voldn { PIN_KEY_VOLDN, HIGH, 0, false, 0 };
static KeyCtx k_volup { PIN_KEY_VOLUP, HIGH, 0, false, 0 };

static void start_rescan()
{
  if (g_rescanning) return;

  g_rescanning = true;
  audio_service_stop(true);
  delay(10);

  ui_scan_begin();

  storage_rescan_flat();
  g_rescan_done = true;
}

static void handle_key(KeyCtx& k,
                       void (*on_short)(),
                       void (*on_long)(),
                       bool repeat = false,
                       void (*on_repeat)() = nullptr)
{
  uint32_t now = millis();
  int s = digitalRead(k.pin);

  // 边沿检测
  if (s != k.last) {
    k.last = s;
    if (pressed(s)) {
      k.t_down = now;
      k.long_fired = false;
      k.t_repeat = now;
      // 音量按键按下时立即通知UI
      if (repeat) ui_volume_key_pressed();
    } else {
      // 松开：短按触发
      if (!k.long_fired && (now - k.t_down) > 25) {
        if (on_short) on_short();
      }
    }
  }

  // 长按触发一次
  if (pressed(k.last) && !k.long_fired && (now - k.t_down) >= 800) {
    k.long_fired = true;
    if (on_long) on_long();
  }

  // 按住连发（音量）
  if (repeat && pressed(k.last) && on_repeat) {
    if (now - k.t_repeat >= 150) {
      k.t_repeat = now;
      on_repeat();
    }
  }
}

void keys_init()
{
  pinMode(PIN_KEY_MODE,  INPUT_PULLUP);
  pinMode(PIN_KEY_PLAY,  INPUT_PULLUP);
  pinMode(PIN_KEY_PREV,  INPUT_PULLUP);
  pinMode(PIN_KEY_NEXT,  INPUT_PULLUP);
  pinMode(PIN_KEY_VOLDN, INPUT_PULLUP);
  pinMode(PIN_KEY_VOLUP, INPUT_PULLUP);
}

void keys_update()
{
  // MODE：短按=随机；长按=重扫
  handle_key(k_mode,  [](){ ui_mode_switch_highlight(); player_toggle_random(); }, start_rescan);

  // PLAY：短按=播放/停止；长按=切换视图
  handle_key(k_play,  player_toggle_play, ui_toggle_view);

  // PREV / NEXT：短按=切歌，长按 NEXT=切换歌手/专辑组
  handle_key(k_prev,  player_prev_track, nullptr);
  handle_key(k_next,  player_next_track, player_next_group);

  // VOL：按住连发
  handle_key(k_voldn, nullptr, nullptr, true, [](){ player_volume_step(-5); });
  handle_key(k_volup, nullptr, nullptr, true, [](){ player_volume_step(+5); });
}