#pragma once
#include <stdbool.h>

// 启动音频专用任务（双核）：AudioTask 会独占 audio_* 接口并持续调用 audio_loop()
void audio_service_start(void);

// wait=true: 阻塞等待命令执行完成（用于 stop 后立刻读封面/扫描，避免 SD 并发）
bool audio_service_play(const char* path, bool wait);
bool audio_service_stop(bool wait);

// 播放状态（由 AudioTask 维护）
bool audio_service_is_playing(void);

// 暂停控制接口
void audio_service_pause(void);
void audio_service_resume(void);
bool audio_service_is_paused(void);

// 淡入淡出控制
float audio_service_get_fade_gain(void);
