#pragma once
#include <stdbool.h>
#include <SdFat.h>

bool audio_mp3_start(SdFat& sd, const char* path);
void audio_mp3_stop();
bool audio_mp3_loop(); // 解码一段并输出，返回是否还在播放
int  audio_mp3_sample_rate();
