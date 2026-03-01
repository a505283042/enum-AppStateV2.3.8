#include "player_state.h"
#include <Arduino.h>
#include "ui/ui.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "storage/storage_music.h"
#include "utils/log.h"
#include "app_flags.h"
#include "lyrics/lyrics.h"

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

// 当前播放的分组索引（歌手/专辑模式）
static int s_current_group_idx = 0;
static int s_current_track_in_group = 0;

// 根据当前歌曲查找所属的歌手组索引
static int find_current_artist_group(void)
{
  const auto& tracks_list = storage_get_tracks();
  if (tracks_list.empty()) return 0;

  auto groups = storage_get_artist_groups();
  String current_artist = tracks_list[s_cur].artist.isEmpty() ? "未知歌手" : tracks_list[s_cur].artist;

  // 如果有多个歌手，取第一个作为主歌手
  int slash_pos = current_artist.indexOf('/');
  if (slash_pos > 0) {
    current_artist = current_artist.substring(0, slash_pos);
    current_artist.trim();
  }

  for (size_t i = 0; i < groups.size(); i++) {
    if (groups[i].name == current_artist) {
      return i;
    }
  }
  return 0;
}

// 根据当前歌曲查找所属的专辑组索引
static int find_current_album_group(void)
{
  const auto& tracks_list = storage_get_tracks();
  if (tracks_list.empty()) return 0;

  auto groups = storage_get_album_groups();
  String current_artist = tracks_list[s_cur].artist.isEmpty() ? "未知歌手" : tracks_list[s_cur].artist;
  String current_album = tracks_list[s_cur].album.isEmpty() ? "未知专辑" : tracks_list[s_cur].album;
  String current_key = current_artist + " - " + current_album;

  for (size_t i = 0; i < groups.size(); i++) {
    if (groups[i].name == current_key) {
      return i;
    }
  }
  return 0;
}

// 获取当前播放列表（根据播放模式）
static std::vector<int> get_current_playlist(void)
{
  std::vector<int> playlist;
  const auto& tracks_list = storage_get_tracks();

  switch (g_play_mode) {
    case PLAY_MODE_ALL_SEQ:
    case PLAY_MODE_ALL_RND:
      for (int i = 0; i < (int)tracks_list.size(); i++) {
        playlist.push_back(i);
      }
      break;

    case PLAY_MODE_ARTIST_SEQ:
    case PLAY_MODE_ARTIST_RND: {
      auto groups = storage_get_artist_groups();
      if (s_current_group_idx >= (int)groups.size()) s_current_group_idx = 0;
      if (!groups.empty()) {
        playlist = groups[s_current_group_idx].track_indices;
      }
      break;
    }

    case PLAY_MODE_ALBUM_SEQ:
    case PLAY_MODE_ALBUM_RND: {
      auto groups = storage_get_album_groups();
      if (s_current_group_idx >= (int)groups.size()) s_current_group_idx = 0;
      if (!groups.empty()) {
        playlist = groups[s_current_group_idx].track_indices;
      }
      break;
    }
  }

  return playlist;
}

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

    // ✅ 封面复用：同一首歌的"停止后再播放"不必重复解码
    bool need_decode_cover = (force_cover || s_cover_idx != s_cur);
    if (need_decode_cover) {
        // 步骤1：从 SD 读取封面到内存（SD 卡操作）
        if (!ui_cover_load_to_memory(t)) {
            ui_draw_cover_for_track(t, true);
        }
        s_cover_idx = s_cur;
    }
    
    // ✅ 在封面缩放（不访问 SD）的同时加载歌词
    // 封面缩放只使用 CPU，此时 SD 卡空闲，可以并行加载歌词
    
    struct LrcTaskParam {
        const char* path;
        volatile bool done;
    };
    
    LrcTaskParam lrc_param = { t.lrc_path.c_str(), false };
    
    if (t.lrc_path.length() > 0) {
        xTaskCreate([](void* param) {
            LrcTaskParam* p = (LrcTaskParam*)param;
            g_lyricsDisplay.loadFromPath(p->path);
            p->done = true;
            vTaskDelete(nullptr);
        }, "lrc_load", 4096, &lrc_param, 1, nullptr);
    }
    
    // 封面缩放（此时歌词加载在另一个任务并行执行）
    if (need_decode_cover) {
        ui_cover_scale_from_memory();
    }
    
    // 等待歌词加载完成
    if (t.lrc_path.length() > 0) {
        while (!lrc_param.done) {
            vTaskDelay(1);
        }
    } else {
        g_lyricsDisplay.clear();
    }

    // ✅ 封面准备好后，再更新文字（这样文字不会先叠在旧封面上）
    ui_set_now_playing(t.title.c_str(), t.artist.c_str());
    ui_set_album(t.album);
    
    // 根据播放模式显示正确的歌曲索引和总数
    int display_pos = s_cur;
    int display_total = (int)tracks_list.size();
    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
        g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        std::vector<int> playlist = get_current_playlist();
        display_total = (int)playlist.size();
        // 查找当前歌曲在组内的位置（1-based）
        for (int i = 0; i < (int)playlist.size(); i++) {
            if (playlist[i] == s_cur) {
                display_pos = i;  // 0-based 索引
                break;
            }
        }
    }
    ui_set_track_pos(display_pos, display_total);
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());

    // ✅ 解冻 UI，让 UI 先显示封面（歌词稍后加载）
    ui_hold_render(false);
    
    // 加载歌词文件（UI 已解冻，用户可以先看到封面）
    if (t.lrc_path.length() > 0) {
        g_lyricsDisplay.loadFromPath(t.lrc_path.c_str());
    } else {
        g_lyricsDisplay.clear();
    }

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
  bool is_random = (g_play_mode == PLAY_MODE_ALL_RND || 
                    g_play_mode == PLAY_MODE_ARTIST_RND || 
                    g_play_mode == PLAY_MODE_ALBUM_RND);

  if (is_random) {
    if (s_random_table_size > 0) {
      next = s_random_table[s_random_table_pos];
      s_random_table_pos = (s_random_table_pos + 1) % s_random_table_size;
    } else {
      next = s_cur;
    }
  } else {
    std::vector<int> playlist = get_current_playlist();
    if (playlist.empty()) return;

    int current_pos = -1;
    for (int i = 0; i < (int)playlist.size(); i++) {
      if (playlist[i] == s_cur) {
        current_pos = i;
        break;
      }
    }

    if (current_pos >= 0) {
      current_pos++;
      if (current_pos >= (int)playlist.size()) {
        current_pos = 0;
      }
      next = playlist[current_pos];
    } else {
      next = s_cur + 1;
      if (next >= (int)tracks_list.size()) next = 0;
    }
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
        std::vector<int> playlist = get_current_playlist();
        if (playlist.empty()) return;

        int current_pos = -1;
        for (int i = 0; i < (int)playlist.size(); i++) {
          if (playlist[i] == s_cur) {
            current_pos = i;
            break;
          }
        }

        if (current_pos >= 0) {
          current_pos--;
          if (current_pos < 0) {
            current_pos = (int)playlist.size() - 1;
          }
          prev = playlist[current_pos];
        } else {
          prev = s_cur - 1;
          if (prev < 0) prev = (int)tracks_list.size() - 1;
        }
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

// 长按 NEXT：切换到下一个歌手/专辑组
void player_next_group()
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;

    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND) {
        auto groups = storage_get_artist_groups();
        if (groups.empty()) return;

        // 切换到下一个歌手组
        s_current_group_idx = (s_current_group_idx + 1) % (int)groups.size();
        String next_artist = groups[s_current_group_idx].name;

        // 播放该组的第一个歌曲
        if (!groups[s_current_group_idx].track_indices.empty()) {
            int next_track = groups[s_current_group_idx].track_indices[0];
            LOGI("[PLAYER] 切换到歌手组: %s (%d/%d)",
                 next_artist.c_str(), s_current_group_idx + 1, (int)groups.size());
            player_play_idx(next_track, false, true);
        }
    }
    else if (g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        auto groups = storage_get_album_groups();
        if (groups.empty()) return;

        // 切换到下一个专辑组
        s_current_group_idx = (s_current_group_idx + 1) % (int)groups.size();
        String next_album = groups[s_current_group_idx].name;

        // 播放该组的第一个歌曲
        if (!groups[s_current_group_idx].track_indices.empty()) {
            int next_track = groups[s_current_group_idx].track_indices[0];
            LOGI("[PLAYER] 切换到专辑组: %s (%d/%d)",
                 next_album.c_str(), s_current_group_idx + 1, (int)groups.size());
            player_play_idx(next_track, false, true);
        }
    }
    else {
        // 全部模式下，长按 NEXT 跳到最后一首
        LOGI("[PLAYER] 跳到最后一首");
        player_play_idx((int)tracks_list.size() - 1, false, true);
    }
}

void player_toggle_random()
{
    // 在6种播放模式之间循环切换
    g_play_mode = (play_mode_t)((g_play_mode + 1) % 6);

    // 切换到歌手/专辑模式时，根据当前歌曲设置组索引
    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND) {
        s_current_group_idx = find_current_artist_group();
    }
    else if (g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        s_current_group_idx = find_current_album_group();
    }

    // 更新随机播放标志
    bool is_random = (g_play_mode == PLAY_MODE_ALL_RND ||
                      g_play_mode == PLAY_MODE_ARTIST_RND ||
                      g_play_mode == PLAY_MODE_ALBUM_RND);

    if (is_random) {
        std::vector<int> playlist = get_current_playlist();
        if (!playlist.empty()) {
            int total = (int)playlist.size();
            s_random_table_size = (total > RANDOM_TABLE_MAX) ? RANDOM_TABLE_MAX : total;
            s_random_table_pos = 0;

            if (total > RANDOM_TABLE_MAX) {
                LOGW("[PLAYER] 随机表容量=%d，但曲目=%d，已截断到前 %d 首（防止溢出）",
                     RANDOM_TABLE_MAX, total, s_random_table_size);
            }

            for (int i = 0; i < s_random_table_size; i++) {
                s_random_table[i] = playlist[i];
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

    // 切换到歌手/专辑模式时，立即更新列表数和索引显示
    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
        g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        const auto& tracks_list = storage_get_tracks();
        if (!tracks_list.empty()) {
            std::vector<int> playlist = get_current_playlist();
            int display_total = (int)playlist.size();
            int display_pos = 0;
            // 查找当前歌曲在组内的位置
            for (int i = 0; i < (int)playlist.size(); i++) {
                if (playlist[i] == s_cur) {
                    display_pos = i;
                    break;
                }
            }
            ui_set_track_pos(display_pos, display_total);
        }
    } else {
        // 全部模式：显示全局索引
        const auto& tracks_list = storage_get_tracks();
        if (!tracks_list.empty()) {
            ui_set_track_pos(s_cur, (int)tracks_list.size());
        }
    }
}