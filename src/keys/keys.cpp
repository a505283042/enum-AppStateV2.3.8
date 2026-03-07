#include <Arduino.h>
#include "keys/keys.h"
#include "keys/keys_pins.h"

#include "app_flags.h"
#include "storage/storage_music.h"
#include "ui/ui.h"
#include "audio/audio_service.h"
#include "player_state.h"
#include "utils/log.h"

// 你项目里这些函数/接口要能被调用
void player_next_track();
void player_prev_track();
void player_toggle_random();
void player_toggle_play();
void player_volume_step(int delta); // 下面我会给你 player_state.cpp 里加
void player_next_group(); // 长按 NEXT：进入列表选择模式

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
  
  // ✅ 安全操作：确保音频服务完全停止后再开始扫描
  // audio_service_stop(true) 只等待音频任务确认收到停止命令
  // 但音频任务可能还需要时间释放文件句柄和 SD 卡互斥锁
  // 因此需要等待 audio_service_is_playing() 返回 false
  audio_service_stop(true);
  
  // 等待音频任务完全停止，最多等待 1 秒
  uint32_t start = millis();
  while (audio_service_is_playing() && (millis() - start) < 1000) {
    delay(10);
  }

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
  // ✅ 渐进式连发：按住时间越长，音量变动越快
  if (repeat && pressed(k.last) && on_repeat) {
    uint32_t hold_time = now - k.t_down;
    uint32_t repeat_interval = 150; // 默认 150ms 间隔
    
    // 按住超过 2 秒后加速到 50ms 间隔
    if (hold_time > 2000) {
      repeat_interval = 50;
    }
    
    if (now - k.t_repeat >= repeat_interval) {
      k.t_repeat = now;
      on_repeat();
    }
  }

  // ✅ 防止长时间按键扫描逻辑阻塞系统
  yield();
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

void keys_reset_all()
{
  k_mode.last = HIGH;
  k_mode.long_fired = false;
  k_mode.t_down = 0;
  k_mode.t_repeat = 0;

  k_play.last = HIGH;
  k_play.long_fired = false;
  k_play.t_down = 0;
  k_play.t_repeat = 0;

  k_prev.last = HIGH;
  k_prev.long_fired = false;
  k_prev.t_down = 0;
  k_prev.t_repeat = 0;

  k_next.last = HIGH;
  k_next.long_fired = false;
  k_next.t_down = 0;
  k_next.t_repeat = 0;

  k_voldn.last = HIGH;
  k_voldn.long_fired = false;
  k_voldn.t_down = 0;
  k_voldn.t_repeat = 0;

  k_volup.last = HIGH;
  k_volup.long_fired = false;
  k_volup.t_down = 0;
  k_volup.t_repeat = 0;
}

void keys_update()
{
  // --- 新增：扫描状态下的紧急处理 ---
  if (g_rescanning) {
    // 在扫描时，我们只关心 MODE 键是否被按下以取消
    int s = digitalRead(k_mode.pin);
    if (s == LOW) { // 只要按下 MODE
      g_abort_scan = true;
      LOGI("[KEYS] Abort signal sent!");
    }
    return; // 扫描时屏蔽其他按键逻辑
  }

  // 检查是否处于列表选择模式
  if (player_is_in_list_select_mode()) {
    // 列表选择模式下的按键处理
    // MODE：短按=返回；长按=取消选择
    handle_key(k_mode,  [](){ player_handle_list_select_key(KEY_MODE_SHORT); }, [](){ player_handle_list_select_key(KEY_MODE_LONG); });

    // PLAY：短按=确认选择
    handle_key(k_play,  [](){ player_handle_list_select_key(KEY_PLAY_SHORT); }, nullptr);

    // PREV / NEXT：短按=上下移动选择
    handle_key(k_prev,  [](){ player_handle_list_select_key(KEY_PREV_SHORT); }, nullptr);
    handle_key(k_next,  [](){ player_handle_list_select_key(KEY_NEXT_SHORT); }, nullptr);

    // VOL：短按=快速翻页
    handle_key(k_voldn, [](){ player_handle_list_select_key(KEY_VOLDN_SHORT); }, nullptr);
    handle_key(k_volup, [](){ player_handle_list_select_key(KEY_VOLUP_SHORT); }, nullptr);
    return;
  }

  // 正常播放模式
  // MODE：短按=随机；长按=重扫
  handle_key(k_mode,  [](){ ui_mode_switch_highlight(); player_toggle_random(); }, start_rescan);

  // PLAY：短按=播放/停止；长按=切换视图
  handle_key(k_play,  player_toggle_play, ui_toggle_view);

  // PREV / NEXT：短按=切歌，长按 NEXT=进入列表选择模式
  handle_key(k_prev,  player_prev_track, nullptr);
  handle_key(k_next,  player_next_track, player_next_group);

  // VOL：按住连发
  handle_key(k_voldn, nullptr, nullptr, true, [](){ player_volume_step(-5); });
  handle_key(k_volup, nullptr, nullptr, true, [](){ player_volume_step(+5); });
}