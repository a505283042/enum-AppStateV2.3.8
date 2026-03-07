#include "storage/storage_music.h"
/* 用户界面系统接口模块头文件 */
#ifndef UI_H
#define UI_H

#include <stdint.h>  /* 包含标准整数类型定义 */

/* =========================================================
 * 用户界面系统接口
 * ========================================================= */

/* UI模式/屏幕类型定义 */
typedef enum {
    UI_SCREEN_BOOT = 0,      /* 启动界面 */
    UI_SCREEN_PLAYER,        /* 播放器界面 */
    UI_SCREEN_NFC_ADMIN,     /* NFC管理界面 */
} ui_screen_t;

/* 播放器视图类型 */
enum ui_player_view_t : uint8_t { UI_VIEW_ROTATE = 0, UI_VIEW_INFO = 1 };

/* 播放模式类型 */
typedef enum {
    PLAY_MODE_ALL_SEQ = 0,    // 全部顺序
    PLAY_MODE_ALL_RND,         // 全部随机
    PLAY_MODE_ARTIST_SEQ,      // 歌手顺序
    PLAY_MODE_ARTIST_RND,      // 歌手随机
    PLAY_MODE_ALBUM_SEQ,       // 专辑顺序
    PLAY_MODE_ALBUM_RND,        // 专辑随机
} play_mode_t;

/* -------- UI生命周期函数 -------- */
void ui_init(void);                        /* 初始化用户界面 */
void ui_set_screen(ui_screen_t screen);    /* 设置当前显示屏幕 */
void ui_update(void);                      /* 更新用户界面 */

/* -------- UI辅助函数 -------- */
void ui_show_message(const char* msg);     /* 显示消息 */

/* -------- UI屏幕进入函数 -------- */
void ui_enter_boot(void);                  /* 进入启动界面 */
void ui_enter_player(void);                /* 进入播放器界面 */
void ui_enter_nfc_admin(void);             /* 进入NFC管理界面 */

bool ui_draw_cover_for_track(const TrackInfo& t, bool force_redraw = false);

// 封面解码拆分（支持并行加载歌词）
bool ui_cover_load_to_memory(const TrackInfo& t);  // 从 SD 读取封面到内存
bool ui_cover_scale_from_memory();                  // 从内存解码缩放到精灵（不访问 SD）

// Player UI相关功能已移除

void ui_show_scanning();
// ===== 扫描 UI =====
void ui_scan_begin();
void ui_scan_tick(int tracks_count);
void ui_scan_end();
void ui_clear_screen();

// ===== 播放器视图切换 =====
void ui_toggle_view();
enum ui_player_view_t ui_get_view();
void ui_set_now_playing(const char* title, const char* artist);
void ui_set_album(const String& album);
void ui_set_volume(uint8_t vol);
void ui_volume_key_pressed();  // 音量按键按下通知
void ui_set_play_mode(play_mode_t mode);  // 设置播放模式
void ui_mode_switch_highlight();  // 模式切换高亮（按键触发）
void ui_set_track_pos(int idx, int total);

void ui_hold_render(bool hold);

// ===== 列表选择界面 =====
void ui_draw_list_select(const std::vector<PlaylistGroup>& groups, int selected_idx, const char* title);
void ui_clear_list_select();

#endif // UI_H