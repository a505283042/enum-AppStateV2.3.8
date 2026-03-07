#include "player_state.h"
#include <Arduino.h>
#include <random>
#include "ui/ui.h"
#include "audio/audio_service.h"
#include "audio/audio.h"
#include "storage/storage_music.h"
#include "utils/log.h"
#include "app_flags.h"
#include "lyrics/lyrics.h"
#include "keys/keys.h"

static bool s_started = false;
static int  s_cur = 0;

// 随机播放相关变量已移除，改为直接打乱 s_current_playlist

// 用户主动暂停/停止时，不自动下一首
static bool s_user_paused = false;

// 暂停功能相关变量
static bool s_is_paused = false;           // 当前是否处于暂停状态
static uint32_t s_pause_time_ms = 0;       // 暂停时的时间戳（用于歌词同步）
static uint32_t s_paused_at_ms = 0;        // 暂停时的播放位置

// ✅ 当前封面缓存属于哪一首；-1 表示未知/需要重解码
static int  s_cover_idx = -1;

// 当前播放的分组索引（歌手/专辑模式）
static int s_current_group_idx = 0;
static int s_current_track_in_group = 0;

// 列表选择模式状态
static ListSelectState s_list_state = ListSelectState::NONE;
static int s_list_selected_idx = 0;
static std::vector<PlaylistGroup> s_list_groups;  // 缓存当前列表

// 基于筛选器的动态列表 + 统一索引缓存
// ⚠️ 内存警告：static 变量分配在全局区域（不占栈），但占用 RAM
// - 2000 首歌约占用 8KB（2000 * 4 bytes）
// - 如果未来支持上万首歌，建议显式分配到 PSRAM
static std::vector<int> s_current_playlist;       // 缓存当前播放列表
static int s_current_playlist_pos = -1;           // 缓存当前歌曲在播放列表中的位置
static play_mode_t s_last_play_mode = PLAY_MODE_ALL_SEQ;  // 上一次的播放模式
static int s_last_group_idx = -1;                 // 上一次的分组索引

// 随机数生成器（静态，避免栈溢出）
static std::mt19937 g_rng(millis());

// 前向声明

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

// 更新当前播放列表缓存
static void update_playlist_cache(void)
{
  const auto& tracks_list = storage_get_tracks();
  bool need_update = false;

  // 检查是否需要更新缓存
  if (g_play_mode != s_last_play_mode) {
    need_update = true;
  } else if ((g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND ||
              g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) &&
             s_current_group_idx != s_last_group_idx) {
    need_update = true;
  }

  if (need_update) {
    s_current_playlist.clear();
    s_current_playlist_pos = -1;

    switch (g_play_mode) {
      case PLAY_MODE_ALL_SEQ:
      case PLAY_MODE_ALL_RND:
        for (int i = 0; i < (int)tracks_list.size(); i++) {
          s_current_playlist.push_back(i);
        }
        break;

      case PLAY_MODE_ARTIST_SEQ:
      case PLAY_MODE_ARTIST_RND: {
        auto groups = storage_get_artist_groups();
        if (s_current_group_idx >= (int)groups.size()) s_current_group_idx = 0;
        if (!groups.empty()) {
          s_current_playlist = groups[s_current_group_idx].track_indices;
        }
        break;
      }

      case PLAY_MODE_ALBUM_SEQ:
      case PLAY_MODE_ALBUM_RND: {
        auto groups = storage_get_album_groups();
        if (s_current_group_idx >= (int)groups.size()) s_current_group_idx = 0;
        if (!groups.empty()) {
          s_current_playlist = groups[s_current_group_idx].track_indices;
        }
        break;
      }
    }

    // 检查是否为随机模式
    bool is_rnd = (g_play_mode == PLAY_MODE_ALL_RND ||
                   g_play_mode == PLAY_MODE_ARTIST_RND ||
                   g_play_mode == PLAY_MODE_ALBUM_RND);

    // 如果是随机模式，打乱列表
    if (is_rnd && !s_current_playlist.empty()) {
      // 洗牌整个列表（使用静态随机数生成器，避免栈溢出）
      std::shuffle(s_current_playlist.begin(), s_current_playlist.end(), g_rng);
    }

    // 尝试找到当前歌曲在新播放列表中的位置
    if (!s_current_playlist.empty()) {
      for (int i = 0; i < (int)s_current_playlist.size(); i++) {
        if (s_current_playlist[i] == s_cur) {
          s_current_playlist_pos = i;
          break;
        }
      }
    }

    // 更新缓存状态
    s_last_play_mode = g_play_mode;
    s_last_group_idx = s_current_group_idx;
  }
}

// 获取当前播放列表（使用缓存机制）
static const std::vector<int>& get_current_playlist(void)
{
  update_playlist_cache();
  return s_current_playlist;
}

// 统一播放入口：切歌时会 stop->解封面->play；恢复播放时尽量复用封面
static void player_play_idx(int idx, bool verbose, bool force_cover)
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;

    if (idx < 0) idx = 0;
    if (idx >= (int)tracks_list.size()) idx = 0;
    s_cur = idx;

    // 更新播放列表缓存和当前位置
    update_playlist_cache();
    if (!s_current_playlist.empty()) {
      for (int i = 0; i < (int)s_current_playlist.size(); i++) {
        if (s_current_playlist[i] == s_cur) {
          s_current_playlist_pos = i;
          break;
        }
      }
    }

    const TrackInfo& t = tracks_list[s_cur];

    // 切歌时重置暂停状态，确保新歌能够正常播放
    s_user_paused = false;
    s_is_paused = false;
    s_pause_time_ms = 0;
    audio_service_resume();  // 确保音频服务的暂停标志被清空

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
        // 显示歌曲的实际索引，而不是播放列表中的位置
        display_pos = s_cur;
    }
    ui_set_track_pos(display_pos, display_total);
    ui_set_play_mode(g_play_mode);
    ui_set_volume(audio_get_volume());

    // ✅ 解冻 UI，让 UI 先显示封面（歌词已在并行任务中加载）
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

        // 强制初始化播放列表缓存，确保切歌功能正常
        s_last_play_mode = (play_mode_t)-1;  // 强制触发缓存更新
        update_playlist_cache();

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
            
            // 强制初始化播放列表缓存，确保切歌功能正常
            s_last_play_mode = (play_mode_t)-1;  // 强制触发缓存更新
            update_playlist_cache();
            
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
        const auto& playlist = get_current_playlist();
        if (playlist.empty()) return;
        
        // 统一使用顺序逻辑，因为随机模式下列表已经被打乱
        int next;
        // 使用缓存的当前位置，避免线性查找
        int current_pos = s_current_playlist_pos;
        
        if (current_pos >= 0 && current_pos < (int)playlist.size() && playlist[current_pos] == s_cur) {
            // 缓存位置有效，直接使用
            current_pos++;
            if (current_pos >= (int)playlist.size()) {
                current_pos = 0;
            }
            next = playlist[current_pos];
        } else {
            // 缓存位置无效，回退到线性查找
            int pos = -1;
            for (int i = 0; i < (int)playlist.size(); i++) {
                if (playlist[i] == s_cur) {
                    pos = i;
                    break;
                }
            }
            
            if (pos >= 0) {
                pos++;
                if (pos >= (int)playlist.size()) {
                    pos = 0;
                }
                next = playlist[pos];
            } else {
                next = s_cur + 1;
                if (next >= (int)tracks_list.size()) next = 0;
            }
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
  // 统一使用顺序逻辑，因为随机模式下列表已经被打乱
  const auto& playlist = get_current_playlist();
  if (playlist.empty()) return;

  // 使用缓存的当前位置，避免线性查找
  int current_pos = s_current_playlist_pos;
  
  if (current_pos >= 0 && current_pos < (int)playlist.size() && playlist[current_pos] == s_cur) {
    // 缓存位置有效，直接使用
    current_pos++;
    if (current_pos >= (int)playlist.size()) {
      current_pos = 0;
    }
    next = playlist[current_pos];
  } else {
    // 缓存位置无效，回退到线性查找
    int pos = -1;
    for (int i = 0; i < (int)playlist.size(); i++) {
      if (playlist[i] == s_cur) {
        pos = i;
        break;
      }
    }

    if (pos >= 0) {
      pos++;
      if (pos >= (int)playlist.size()) {
        pos = 0;
      }
      next = playlist[pos];
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
    // 统一使用顺序逻辑，因为随机模式下列表已经被打乱
    const auto& playlist = get_current_playlist();
    if (playlist.empty()) return;

    // 使用缓存的当前位置，避免线性查找
    int current_pos = s_current_playlist_pos;
    
    if (current_pos >= 0 && current_pos < (int)playlist.size() && playlist[current_pos] == s_cur) {
        // 缓存位置有效，直接使用
        current_pos--;
        if (current_pos < 0) {
            current_pos = (int)playlist.size() - 1;
        }
        prev = playlist[current_pos];
    } else {
        // 缓存位置无效，回退到线性查找
        int pos = -1;
        for (int i = 0; i < (int)playlist.size(); i++) {
            if (playlist[i] == s_cur) {
                pos = i;
                break;
            }
        }

        if (pos >= 0) {
            pos--;
            if (pos < 0) {
                pos = (int)playlist.size() - 1;
            }
            prev = playlist[pos];
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

    // 使用新的暂停/恢复机制
    if (audio_service_is_paused()) {
        // 恢复播放
        audio_service_resume();
        s_is_paused = false;
        s_user_paused = false;
        
        // 恢复歌词计时：计算暂停期间经过的时间，并调整暂停时间戳
        uint32_t pause_duration = millis() - s_pause_time_ms;
        s_pause_time_ms = 0;
        // 注意：歌词显示使用 audio_get_play_ms() 获取时间，
        // 由于音频服务暂停时音频时间也停止，所以不需要额外调整歌词时间
        
        LOGI("[PLAYER] Resumed from pause");
        return;
    }

    if (audio_service_is_playing()) {
        // 暂停播放（而不是停止）
        audio_service_pause();
        s_is_paused = true;
        s_user_paused = true;
        s_pause_time_ms = millis();
        s_paused_at_ms = audio_get_play_ms();
        
        LOGI("[PLAYER] Paused at %u ms", s_paused_at_ms);
        return;
    }

    // 如果既没有播放也没有暂停（停止状态），则开始播放
    // ✅ 恢复播放：尽量复用封面（同一首歌不再重解码）
    s_is_paused = false;
    s_user_paused = false;
    s_pause_time_ms = 0;
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

// 长按 NEXT：进入列表选择模式（歌手/专辑模式下）
void player_next_group()
{
    const auto& tracks_list = storage_get_tracks();
    if (tracks_list.empty()) return;

    // ✅ 重置所有按键状态，防止长按状态残留导致逻辑异常
    keys_reset_all();

    if (g_play_mode == PLAY_MODE_ARTIST_SEQ || g_play_mode == PLAY_MODE_ARTIST_RND) {
        // 进入歌手列表选择模式
        s_list_groups = storage_get_artist_groups();
        if (s_list_groups.empty()) {
            s_list_state = ListSelectState::NONE;
            s_current_group_idx = 0;
            return;
        }
        
        // 设置当前选中项为当前歌手组
        s_list_selected_idx = s_current_group_idx;
        if (s_list_selected_idx >= (int)s_list_groups.size()) {
            s_list_selected_idx = 0;
        }
        
        s_list_state = ListSelectState::ARTIST;
        LOGI("[LIST] 进入歌手列表选择模式，共 %d 个歌手，当前选中: %d", 
             (int)s_list_groups.size(), s_list_selected_idx + 1);
    }
    else if (g_play_mode == PLAY_MODE_ALBUM_SEQ || g_play_mode == PLAY_MODE_ALBUM_RND) {
        // 进入专辑列表选择模式
        s_list_groups = storage_get_album_groups();
        if (s_list_groups.empty()) {
            s_list_state = ListSelectState::NONE;
            s_current_group_idx = 0;
            return;
        }
        
        // 设置当前选中项为当前专辑组
        s_list_selected_idx = s_current_group_idx;
        if (s_list_selected_idx >= (int)s_list_groups.size()) {
            s_list_selected_idx = 0;
        }
        
        s_list_state = ListSelectState::ALBUM;
        LOGI("[LIST] 进入专辑列表选择模式，共 %d 个专辑，当前选中: %d", 
             (int)s_list_groups.size(), s_list_selected_idx + 1);
    }
    else {
        // 全部模式下，长按 NEXT 跳10首
        int next = (s_cur + 10) % tracks_list.size();
        LOGI("[PLAYER] 跳10首 -> #%d", next);
        player_play_idx(next, false, true);
    }
}

// 检查是否处于列表选择模式
bool player_is_in_list_select_mode()
{
    return s_list_state != ListSelectState::NONE;
}

// 获取当前列表选择状态
ListSelectState player_get_list_select_state()
{
    return s_list_state;
}

// 获取当前选中的列表索引
int player_get_list_selected_idx()
{
    return s_list_selected_idx;
}

// 获取当前列表组
const std::vector<PlaylistGroup>& player_get_list_groups()
{
    return s_list_groups;
}

// 处理列表选择模式的按键
void player_handle_list_select_key(key_event_t evt)
{
    if (s_list_state == ListSelectState::NONE) return;
    
    int group_count = (int)s_list_groups.size();
    if (group_count == 0) {
        s_list_state = ListSelectState::NONE;
        return;
    }
    
    switch (evt) {
        case KEY_NEXT_SHORT:
            s_list_selected_idx = (s_list_selected_idx + 1) % group_count;
            LOGI("[LIST] 选择下一项: %d/%d", s_list_selected_idx + 1, group_count);
            break;
            
        case KEY_PREV_SHORT:
            s_list_selected_idx = (s_list_selected_idx - 1 + group_count) % group_count;
            LOGI("[LIST] 选择上一项: %d/%d", s_list_selected_idx + 1, group_count);
            break;
            
        case KEY_VOLUP_SHORT:
            s_list_selected_idx = (s_list_selected_idx + 5) % group_count;
            LOGI("[LIST] 向下翻页: %d/%d", s_list_selected_idx + 1, group_count);
            break;
            
        case KEY_VOLDN_SHORT:
            s_list_selected_idx = (s_list_selected_idx - 5 + group_count) % group_count;
            LOGI("[LIST] 向上翻页: %d/%d", s_list_selected_idx + 1, group_count);
            break;
            
        case KEY_PLAY_SHORT:
            // 确认选择
            s_current_group_idx = s_list_selected_idx;
            LOGI("[LIST] 确认选择: %s (%d/%d)", 
                 s_list_groups[s_current_group_idx].name.c_str(),
                 s_current_group_idx + 1, group_count);
            
            // 播放该组的第一个歌曲
            if (!s_list_groups[s_current_group_idx].track_indices.empty()) {
                int next_track = s_list_groups[s_current_group_idx].track_indices[0];
                s_list_state = ListSelectState::NONE;
                ui_clear_list_select(); // 重置列表选择界面状态
                player_play_idx(next_track, false, true);
            }
            break;
            
        case KEY_MODE_SHORT:
        case KEY_MODE_LONG:
            // 取消选择（短按和长按都执行返回操作）
            LOGI("[LIST] 取消选择");
            s_list_state = ListSelectState::NONE;
            ui_clear_list_select(); // 重置列表选择界面状态
            break;
            
        default:
            break;
    }
}

// 随机表函数已移除，改为直接在 update_playlist_cache 中打乱列表

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
        // 随机播放已开启，列表会在 update_playlist_cache 中自动打乱
        LOGI("[PLAYER] 随机播放: 开启");
    } else {
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
            const auto& playlist = get_current_playlist();
            int display_total = (int)playlist.size();
            // 显示歌曲的实际索引，而不是播放列表中的位置
            int display_pos = s_cur;
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