#include <Arduino.h>
#include <string.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#include "audio/audio_service.h"
#include "audio/audio.h"

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

static void audio_task_entry(void*)
{
  // I2S/decoder 初始化放在音频任务内部，确保由同一线程管理
  if (!audio_init()) {
    Serial.println("[AUDIO] init failed (AudioTask)");
  }

  s_ready = true;

  for (;;) {
    // 1) 先处理队列里的控制命令
    AudioCmd cmd;
    while (s_q && xQueueReceive(s_q, &cmd, 0) == pdTRUE) {
      uint32_t ack = 1;

      if (cmd.type == CMD_STOP) {
        audio_stop();
      } else if (cmd.type == CMD_PLAY) {
        bool ok = audio_play(cmd.path);
        ack = ok ? 1 : 0;
      }

      // 刷新播放状态缓存
      s_playing_cache = audio_is_playing();

      if (cmd.notify_to) {
        xTaskNotify(cmd.notify_to, ack, eSetValueWithOverwrite);
      }
    }

    // 2) 持续喂音频（播放时必须高频）
    audio_loop();

    // 3) 同步缓存：处理"自然播放结束"这种情况
    s_playing_cache = audio_is_playing();

    // 4) CPU 使用优化：
    //    - 播放时：audio_loop 内部的 i2s_write 会阻塞，但仍需小延迟避免 100% 占用
    //    - 空闲时：较长延迟节省 CPU
    if (!s_playing_cache) {
      vTaskDelay(2);  // 空闲时延迟 2ms
    } else {
      vTaskDelay(1);  // 播放时延迟 1ms，确保其他任务有机会运行
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
