#include <Arduino.h>
#include "audio/audio_mp3.h"
#include "audio/audio_i2s.h"
#include "audio/audio_file.h"
#include "utils/log.h"

#define MINIMP3_IMPLEMENTATION
#include "../../lib/minimp3/minimp3.h"

static SdFat* g_sd = nullptr;
static AudioFile g_file;
static mp3dec_t g_dec;

static uint8_t g_inbuf[8 * 1024];
static int g_inbuf_filled = 0;
static bool g_playing = false;
static int g_sr = 44100;

static int16_t g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME]; // stereo interleaved
static size_t s_pending_off = 0;
static size_t s_pending_frames = 0;



bool audio_mp3_start(SdFat& sd, const char* path)
{
  audio_mp3_stop();

  g_sd = &sd;
  if (!g_file.open(sd, path)) {
    LOGE("[MP3] open failed: %s", path);
    return false;
  }

  mp3dec_init(&g_dec);
  g_inbuf_filled = 0;

  // 预读一点
  int n = g_file.read(g_inbuf, sizeof(g_inbuf));
  if (n <= 0) { audio_mp3_stop(); return false; }
  g_inbuf_filled = n;

  g_playing = true;
  g_sr = 44100;
  return true;
}

void audio_mp3_stop()
{
    // ✅ 清 pending PCM（非常重要）
    s_pending_off = 0;
    s_pending_frames = 0;

    if (g_file.f) g_file.close();
    g_playing = false;
    g_inbuf_filled = 0;
}

int audio_mp3_sample_rate() { return g_sr; }

bool audio_mp3_loop()
{
    if (!g_playing) return false;

    // --- A) 先把 pending 的 PCM 写完 ---
    if (s_pending_frames > 0) {
        size_t w = audio_i2s_write_frames(g_pcm + s_pending_off * 2, s_pending_frames);
        if (w == SIZE_MAX) { audio_mp3_stop(); return false; }
        s_pending_off    += w;
        s_pending_frames -= w;

        // 这轮先不解新帧，尽快回到主循环（高频喂）
        return true;
    }

    // --- B) 输入补充 ---
    if (g_inbuf_filled < 2048) {
        int space = (int)sizeof(g_inbuf) - g_inbuf_filled;
        if (space > 0) {
            int n = g_file.read(g_inbuf + g_inbuf_filled, space);
            if (n > 0) g_inbuf_filled += n;
        }
        if (g_inbuf_filled == 0) { audio_mp3_stop(); return false; }
    }

    // --- C) 解一帧 ---
    mp3dec_frame_info_t info;
    int samples = mp3dec_decode_frame(&g_dec, g_inbuf, g_inbuf_filled, g_pcm, &info);

    if (info.frame_bytes == 0) {
        if (g_inbuf_filled == (int)sizeof(g_inbuf)) {
            memmove(g_inbuf, g_inbuf + 1024, g_inbuf_filled - 1024);
            g_inbuf_filled -= 1024;
        }
        return true;
    }

    // --- D) 消费输入 ---
    if (info.frame_bytes > 0 && info.frame_bytes <= g_inbuf_filled) {
        memmove(g_inbuf, g_inbuf + info.frame_bytes, g_inbuf_filled - info.frame_bytes);
        g_inbuf_filled -= info.frame_bytes;
    } else {
        audio_mp3_stop();
        return false;
    }

    // --- E) 写 PCM（建立 pending） ---
    if (samples > 0) {
        g_sr = info.hz;
        static int last_sr = 0;
        if (g_sr != last_sr) {
            audio_i2s_set_sample_rate(g_sr);
            last_sr = g_sr;
        }

        s_pending_off = 0;
        s_pending_frames = (size_t)samples;

        // 先尝试写一次，写不完就留 pending
        size_t w = audio_i2s_write_frames(g_pcm, s_pending_frames);
        if (w == SIZE_MAX) { audio_mp3_stop(); return false; }
        s_pending_off    += w;
        s_pending_frames -= w;
    }

    return true;
}
