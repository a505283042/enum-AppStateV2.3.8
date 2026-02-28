#pragma once
#include <Arduino.h>
#include <SdFat.h>

struct FlacCoverLoc {
  bool found = false;
  uint32_t offset = 0;
  uint32_t size = 0;
  String mime;
};

bool flac_find_picture(SdFat& sd, const char* path, FlacCoverLoc& out);
