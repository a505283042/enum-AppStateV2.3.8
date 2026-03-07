#include <Arduino.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "audio/audio_service.h"
#include "audio/audio.h"
#include "audio/audio_i2s.h"

#ifndef AUDIO_TASK_CORE
#define AUDIO_TASK_CORE 0   // Arduino loopTask 默认在 core1；把音频钉到 core0 更稳
#endif

#ifndef AUDIO_TASK_PRIO
#define AUDIO_TASK_PRIO 5
#endif

#ifndef AUDIO_TASK_STACK
// 4096 在打开 FLAC（dr_flac）时很容易触发 Stack Canary，调大栈就能解决
#define AUDIO_TASK_STACK 12288
#endif

#ifndef AUDIO_CMD_PATH_MAX
#define AUDIO_CMD_PATH_MAX 260
#endif

enum AudioCmdType : uint8_t { CMD_PLAY = 1, CMD_STOP = 2 };

struct AudioCmd {
  AudioCmdType type;
  char path[AUDIO_CMD_PATH_MAX];
  TaskHandle_t notify_to;   // wait=true 时，用 notify ACK
};

static QueueHandle_t s_q = nullptr;
static TaskHandle_t  s_task = nullptr;
static volatile bool s_playing_cache = false;
static volatile bool s_ready = false;
static bool s_paused = false; // 内部暂停标志

// 淡入淡出功能相关变量
static float s_fade_gain = 1.0f; // 当前增益 (0.0 到 1.0)
static float s_last_fade_gain = 1.0f; // 上一次的增益，用于检测淡出完成
// 这里的速度决定了淡入淡出的快慢：10ms 左右比较合适
#define FADE_STEP 0.05f

static void audio_task_entry(void*)
{
  // I2S/decoder 初始化放在音频任务内部，确保由同一线程管理
  if (!audio_init()) {
    Serial.println("[AUDIO] init failed (AudioTask)");
  }

  s_ready = true;

  for (;;) {
    // 1) 先处理队列里的控制命令（保持不变，这样暂停时依然可以发 CMD_STOP 停止播放）
    AudioCmd cmd;
    while (s_q && xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
      uint32_t ack = 1;

      if (cmd.type == CMD_STOP) {
        audio_stop();
      } else if (cmd.type == CMD_PLAY) {
        bool ok = audio_play(cmd.path);
        ack = ok ? 1 : 0;
        // 收到新歌 PLAY 命令，自动取消暂停
        s_paused = false;
      }

      // 刷新播放状态缓存
      s_playing_cache = audio_is_playing();

      if (cmd.notify_to) {
        xTaskNotify(cmd.notify_to, ack, eSetValueWithOverwrite);
      }
    }

    // --- 核心：淡入淡出状态机 ---
    if (s_paused) {
      if (s_fade_gain > 0.0f) {
        s_fade_gain -= FADE_STEP;
        if (s_fade_gain < 0.0f) s_fade_gain = 0.0f;
      }
    } else {
      if (s_fade_gain < 1.0f) {
        s_fade_gain += FADE_STEP;
        if (s_fade_gain > 1.0f) s_fade_gain = 1.0f;
      }
    }

    // 检测淡出完成，清空 DMA 缓冲区消除底噪
    if (s_last_fade_gain > 0.0f && s_fade_gain == 0.0f) {
      audio_i2s_zero_dma_buffer();
    }
    s_last_fade_gain = s_fade_gain;

    // 只有当增益大于 0，或者虽然暂停但还没淡出完成时，才跑循环
    if (s_fade_gain > 0.0f) {
      // 在这里你需要调用解码库提供的 setVolume 或直接对 PCM 数据乘以 s_fade_gain
      // 如果你的音频库支持，可以这样：
      // audio.setVolumeFactor(s_fade_gain);
      audio_loop();
    }

    // 2) 同步缓存：处理"自然播放结束"这种情况
    bool was_playing = s_playing_cache;
    s_playing_cache = audio_is_playing();

    // 播放结束自动复位：如果一首歌自然播放完了（EOF），自动复位暂停状态
    // 否则下一首歌可能会卡在暂停状态
    if (was_playing && !s_playing_cache) {
      s_paused = false;
      s_fade_gain = 1.0f;
      s_last_fade_gain = 1.0f;
    }

    // 3) 优化延迟：淡入淡出期间使用较短延迟
    if (s_fade_gain > 0.0f && s_fade_gain < 1.0f) {
      vTaskDelay(1);
    } else if (!s_playing_cache || s_paused) {
      vTaskDelay(10);
    } else {
      vTaskDelay(1);
    }
  }
}

void audio_service_start(void)
{
  if (s_task) return;

  s_q = xQueueCreate(8, sizeof(AudioCmd));
  xTaskCreatePinnedToCore(audio_task_entry,
                          "AudioTask",
                          AUDIO_TASK_STACK,
                          nullptr,
                          AUDIO_TASK_PRIO,
                          &s_task,
                          AUDIO_TASK_CORE);
}

static bool wait_ready(uint32_t timeout_ms)
{
  uint32_t t0 = millis();
  while (!s_ready && (millis() - t0) < timeout_ms) {
    vTaskDelay(1);
  }
  return s_ready;
}

static bool send_cmd(AudioCmd& cmd, bool wait)
{
  if (!wait_ready(1000)) return false;
  if (!s_q) return false;

  cmd.notify_to = wait ? xTaskGetCurrentTaskHandle() : nullptr;

  // 使用 100ms 超时，避免 UI 线程卡死
  if (xQueueSend(s_q, &cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println("[AUDIO] 发送命令超时，音频核心忙");
    return false;
  }
  if (!wait) return true;

  uint32_t ack = 0;
  if (xTaskNotifyWait(0, 0xFFFFFFFF, &ack, pdMS_TO_TICKS(2000)) != pdTRUE) return false;
  return ack == 1;
}

bool audio_service_play(const char* path, bool wait)
{
  if (!path) return false;

  AudioCmd cmd{};
  cmd.type = CMD_PLAY;
  strncpy(cmd.path, path, sizeof(cmd.path) - 1);
  cmd.path[sizeof(cmd.path) - 1] = '\0';

  return send_cmd(cmd, wait);
}

bool audio_service_stop(bool wait)
{
  AudioCmd cmd{};
  cmd.type = CMD_STOP;
  cmd.path[0] = 0;
  return send_cmd(cmd, wait);
}

bool audio_service_is_playing(void)
{
  return s_playing_cache;
}

// 供外部调用的暂停控制接口
void audio_service_pause() { 
    s_paused = true; 
    // 注意：不再立即调用 zero_dma_buffer，因为我们要留时间做淡出 
}
void audio_service_resume() { 
    s_paused = false; 
}
bool audio_service_is_paused() { return s_paused; }

// 获取当前淡入淡出增益
float audio_service_get_fade_gain() { return s_fade_gain; }
