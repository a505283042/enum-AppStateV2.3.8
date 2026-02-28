#pragma once

#include <Arduino.h>

// ========== UI颜色常量 ==========

// 文本颜色
#define UI_COLOR_TITLE       0xFFFF  // 歌名（纯白）
#define UI_COLOR_ARTIST      0x9494  // 歌手（浅灰）
#define UI_COLOR_ALBUM       0x8410  // 专辑名（中灰）
#define UI_COLOR_TIME        0xB5B5  // 时间（浅灰）

// 音量激活颜色
#define UI_COLOR_VOLUME_ACTIVE 0xFE20  // 音量激活状态（暖阳橙黄）

// 进度条颜色
#define UI_COLOR_BAR_BG      0x2980  // 进度条未播放（森林绿）
#define UI_COLOR_BAR_PLAY    0xFE20  // 进度条已播放（暖阳橙黄）
#define UI_COLOR_BAR_CURSOR  0xFC1F  // 进度条光标头（活力紫红）
