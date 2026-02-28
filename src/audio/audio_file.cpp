#include "audio/audio_file.h"

bool AudioFile::open(SdFat& sd, const char* path) {
  f = sd.open(path, O_RDONLY);
  return (bool)f;
}
void AudioFile::close() { if (f) f.close(); }
int AudioFile::read(void* dst, size_t bytes) {
  if (!f) return -1;
  int n = f.read(dst, bytes);
  return n < 0 ? -1 : n;
}
bool AudioFile::seek(uint32_t pos) { return f && f.seekSet(pos); }
uint32_t AudioFile::tell() { return f ? (uint32_t)f.curPosition() : 0; }
uint32_t AudioFile::size() { return f ? (uint32_t)f.fileSize() : 0; }
