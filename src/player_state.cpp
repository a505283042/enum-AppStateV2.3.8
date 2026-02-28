#include "player_state.h"
#include <Arduino.h>
#include "ui/ui.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "storage/storage_music.h"
#include "utils/log.h"
#include "app_flags.h"

static bool s_started = false;
static int  s_cur = 0;

// 随机索引表
static int  s_random_table[1024];
static int  s_random_table_size = 0;
static int  s_random_table_pos  = 0;
static constexpr int RANDOM_TABLE_MAX = sizeof(s_random_table) / sizeof(s_random_table[0]);

// 用户主动暂停/停止时，不自动下一首
static bool s_user_paused = false;

// ✅ 当前封面缓存属于哪一首；-1 表示未知/需要重解码
static int  s_cover_idx = -1;

// 统一播放入口：切歌时会 stop->解封面->play；恢复播放时尽量复用封面
static void player_play_idx(int idx, bool verbose, bool force_cover)
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;

    if (idx < 0) idx = 0;
    if (idx >= (int)tracks_list.size()) idx = 0;
    s_cur = idx;

    const TrackInfo& t = tracks_list[s_cur];

    s_user_paused = false;

    LOGI("[PLAYER] play #%d: %s", s_cur, t.audio_path.c_str());
    // ✅ 先冻结 UI，避免新文字画在旧封面上
    ui_hold_render(true);

    if (verbose) {
        Serial.println("----- TRACK META CHECK -----");
        Serial.printf("path  : %s\n", t.audio_path.c_str());
        Serial.printf("ext   : %s\n", t.ext.c_str());
        Serial.printf("artist: %s\n", t.artist.c_str());
        Serial.printf("album : %s\n", t.album.c_str());
        Serial.printf("title : %s\n", t.title.c_str());
        Serial.println("---------------------------");

        Serial.printf("cover_source=%d offset=%u size=%u mime=%s path=%s\n",
                      (int)t.cover_source,
                      (unsigned)t.cover_offset,
                      (unsigned)t.cover_size,
                      t.cover_mime.c_str(),
                      t.cover_path.c_str());
    }

    // ✅ 切歌/需要读封面时必须先停音频（避免 SD 并发）
    if (audio_service_is_playing()) {
        audio_service_stop(true);
    }

    // ✅ 封面复用：同一首歌的“停止后再播放”不必重复解码
    if (force_cover || s_cover_idx != s_cur) {
        ui_draw_cover_for_track(t, true);
        s_cover_idx = s_cur;
    }

    // ✅ 封面准备好后，再更新文字（这样文字不会先叠在旧封面上）
    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    ui_set_album(t.album);
    ui_set_track_pos(s_cur, (int)tracks_list.size());
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());

    // ✅ 解冻 UI，让 UiTask 下一帧开始用"新封面+新文字"绘制
    ui_hold_render(false);

    if (!audio_service_play(t.audio_path.c_str(), true)) {
        LOGE("[AUDIO] play failed");
        return;
    }

    s_started = true;
}

void player_state_run(void)
{
    static bool entered = false;

    const auto& tracks_list = storage_get_tracks();

    if (!entered) {
        entered = true;
        LOGI("[PLAYER] enter");
        ui_enter_player();
        s_cover_idx = -1;

        LOGI("[SCAN] albums=%d tracks=%d",
             (int)storage_get_albums().size(),
             (int)tracks_list.size());

        if (!tracks_list.empty()) {
            player_play_idx(0, true, true);
        } else {
            LOGI("[PLAYER] no tracks");
        }
        return;
    }

    // ✅ 扫描完成：回到播放器并自动播第0首
    if (g_rescan_done) {
        g_rescan_done = false;

        const auto& tl = storage_get_tracks();
        g_rescanning = false;

        if (!tl.empty()) {
            ui_enter_player();
            s_cover_idx = -1;                 // 强制封面重解码，避免闪旧封面/缓存不一致
            player_play_idx(0, true, true);
        } else {
            LOGI("[PLAYER] no tracks after rescan");
        }
        return;
    }

    // ✅ 扫描期间：禁止任何自动切歌/播放逻辑
    if (g_rescanning) return;

    // ✅ 播放自然结束 -> 下一首（循环或随机）
    if (entered && s_started && !tracks_list.empty() && !s_user_paused && !audio_service_is_playing()) {
        int next;
        if (g_random_play) {
            if (s_random_table_size > 0) {
                next = s_random_table[s_random_table_pos];
                s_random_table_pos = (s_random_table_pos + 1) % s_random_table_size;
            } else {
                next = s_cur;
            }
        } else {
            next = s_cur + 1;
            if (next >= (int)tracks_list.size()) next = 0;
        }
        player_play_idx(next, false, true);
    }

    // UiTask 已负责旋转推屏，这里不再 ui_update()
}

void player_next_track()
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;

    int next;
    if (g_random_play) {
        if (s_random_table_size > 0) {
            next = s_random_table[s_random_table_pos];
            s_random_table_pos = (s_random_table_pos + 1) % s_random_table_size;
        } else {
            next = s_cur;
        }
    } else {
        next = s_cur + 1;
        if (next >= (int)tracks_list.size()) next = 0;
    }

    LOGI("[PLAYER] NEXT -> #%d", next);
    player_play_idx(next, false, true);
}

void player_prev_track()
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;

    int prev;
    if (g_random_play) {
        if (s_random_table_size > 0) {
            // 随机模式：位置指针总是指向下一首，所以要回退两次
            // 第一次回退到当前歌曲的位置，第二次回退到上一首
            int prev_pos = (s_random_table_pos - 2 + s_random_table_size) % s_random_table_size;
            prev = s_random_table[prev_pos];
            // 更新位置指针到上一首的位置，这样下次按“下一曲”时会从正确位置开始
            s_random_table_pos = (prev_pos + 1) % s_random_table_size;
        } else {
            prev = s_cur;
        }
    } else {
        // 顺序模式：使用当前索引的前一个
        prev = s_cur - 1;
        if (prev < 0) prev = (int)tracks_list.size() - 1;
    }

    LOGI("[PLAYER] PREV -> #%d", prev);
    // ✅ 不再 ui_enter_player()，避免额外清屏/复位造成“更慢”
    player_play_idx(prev, false, true);
}

void player_toggle_play()
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;
    if (g_rescanning) return;

    if (audio_service_is_playing()) {
        s_user_paused = true;
        audio_service_stop(true);
        return;
    }

    // ✅ 恢复播放：尽量复用封面（同一首歌不再重解码）
    s_user_paused = false;
    player_play_idx(s_cur, false, false);
}

void player_volume_step(int delta)
{
    int v = (int)audio_get_volume() + delta;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    audio_set_volume((uint8_t)v);

    LOGI("[VOL] %d%%", v);
    ui_set_volume((uint8_t)v);
}

void player_toggle_random()
{
    // 在6种播放模式之间循环切换
    g_play_mode = (play_mode_t)((g_play_mode + 1) % 6);
    
    // 更新随机播放标志
    bool is_random = (g_play_mode == PLAY_MODE_ALL_RND || 
                      g_play_mode == PLAY_MODE_ARTIST_RND || 
                      g_play_mode == PLAY_MODE_ALBUM_RND);
    
    if (is_random) {
        const auto& tracks_list = storage_get_tracks();
        if (!tracks_list.empty()) {
            int total = (int)tracks_list.size();
            s_random_table_size = (total > RANDOM_TABLE_MAX) ? RANDOM_TABLE_MAX : total;
            s_random_table_pos = 0;

            if (total > RANDOM_TABLE_MAX) {
                LOGW("[PLAYER] 随机表容量=%d，但曲目=%d，已截断到前 %d 首（防止溢出）",
                     RANDOM_TABLE_MAX, total, s_random_table_size);
            }

            for (int i = 0; i < s_random_table_size; i++) {
                s_random_table[i] = i;
            }
            for (int i = s_random_table_size - 1; i > 0; i--) {
                int j = random(i + 1);
                int temp = s_random_table[i];
                s_random_table[i] = s_random_table[j];
                s_random_table[j] = temp;
            }

            LOGI("[PLAYER] 随机播放: 开启 (索引表大小: %d)", s_random_table_size);
        } else {
            s_random_table_size = 0;
            LOGI("[PLAYER] 随机播放: 开启 (无歌曲)");
        }
    } else {
        s_random_table_size = 0;
        LOGI("[PLAYER] 随机播放: 关闭");
    }
    
    // 同步更新UI
    g_random_play = is_random;
    ui_set_play_mode(g_play_mode);
}