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

static int16_t g_pcm[MINIMP3_MAX_SAMPLES_PER_FRAME * 2]; // stereo interleaved (预留双声道空间)
static size_t s_pending_off = 0;
static size_t s_pending_frames = 0;
static int s_channels = 2; // 当前声道数
static int s_last_sr = 0; // 上次设置的采样率（文件级 static，便于重置）



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
  s_last_sr = 0; // 重置采样率缓存，确保新文件一定会设置 I2S 时钟
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
        // 只要有数据且没解出帧，就尝试找同步
        if (g_inbuf_filled >= 2) {
            int sync_pos = -1;
            // 搜索缓冲区，寻找 0xFF (Syncword 开始)
            // 注意：从 1 开始搜，因为 0 位置已经证明解不出帧了
            for (int i = 1; i < g_inbuf_filled - 1; ++i) {
                if (g_inbuf[i] == 0xFF && (g_inbuf[i+1] & 0xE0) == 0xE0) {
                    sync_pos = i;
                    break;
                }
            }

            if (sync_pos > 0) {
                // 移位到新的同步点
                memmove(g_inbuf, g_inbuf + sync_pos, g_inbuf_filled - sync_pos);
                g_inbuf_filled -= sync_pos;
                LOGD("[MP3] Resynced to pos %d", sync_pos);
            } else {
                // 彻底没找到，保留最后 1 字节（防止切断跨缓冲区的同步头），其余丢弃
                int keep = 1;
                memmove(g_inbuf, g_inbuf + g_inbuf_filled - keep, keep);
                g_inbuf_filled = keep;
            }
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

    // --- E) 处理单声道/双声道 ---
    if (samples > 0) {
        g_sr = info.hz;
        s_channels = info.channels;
        if (g_sr != s_last_sr) {
            audio_i2s_set_sample_rate(g_sr);
            s_last_sr = g_sr;
        }

        // 如果是单声道，扩充为双声道（复制到左右声道）
        if (s_channels == 1) {
            // 从后往前复制，避免覆盖
            for (int i = samples - 1; i >= 0; --i) {
                g_pcm[i * 2] = g_pcm[i];     // 左声道
                g_pcm[i * 2 + 1] = g_pcm[i]; // 右声道
            }
            samples *= 2; // 样本数翻倍（现在是立体声样本数）
        }

        // --- F) 写 PCM（建立 pending） ---
        s_pending_off = 0;
        s_pending_frames = (size_t)(samples / 2); // 转换为帧数（每帧2个样本）

        // 先尝试写一次，写不完就留 pending
        size_t w = audio_i2s_write_frames(g_pcm, s_pending_frames);
        if (w == SIZE_MAX) { audio_mp3_stop(); return false; }
        s_pending_off    += w;
        s_pending_frames -= w;
    }

    return true;
}
