#pragma once
#include <Arduino.h>
#include <SdFat.h>
#include "gc9a01_lgfx.h"
#include "storage/storage_music.h"

// 把封面数据“导出”为一个临时图片文件（jpg/png），成功返回 true
bool cover_export_to_temp(SdFat& sd, const TrackInfo& t, const char* temp_path);

// 直接画封面到屏幕指定区域（内部会先 export 到 temp 再 draw）
bool ui_draw_cover(LGFX& tft, SdFat& sd, const TrackInfo& t, int x, int y, int w, int h);
