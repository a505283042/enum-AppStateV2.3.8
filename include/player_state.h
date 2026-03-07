/* 播放器状态管理模块头文件 */
#ifndef PLAYER_STATE_H
#define PLAYER_STATE_H

#include "storage/storage_music.h"
#include <vector>

/* 列表选择状态 */
enum class ListSelectState {
    NONE,       // 正常播放模式
    ARTIST,     // 选择歌手
    ALBUM       // 选择专辑
};

/* 按键事件类型 */
enum key_event_t {
    KEY_NEXT_SHORT,
    KEY_PREV_SHORT,
    KEY_PLAY_SHORT,
    KEY_MODE_SHORT,
    KEY_MODE_LONG,
    KEY_VOLUP_SHORT,
    KEY_VOLDN_SHORT
};

/* 播放器状态处理函数声明 */
void player_state_run(void);  /* 运行播放器状态处理函数 */
void player_next_track();
void player_toggle_random();  /* 切换随机播放模式 */
void player_prev_track();
void player_toggle_play();
void player_volume_step(int delta);
void player_next_group();     /* 长按 NEXT：进入列表选择模式 */

/* 列表选择模式函数 */
bool player_is_in_list_select_mode();                          /* 检查是否处于列表选择模式 */
ListSelectState player_get_list_select_state();                /* 获取当前列表选择状态 */
int player_get_list_selected_idx();                            /* 获取当前选中的列表索引 */
const std::vector<PlaylistGroup>& player_get_list_groups();    /* 获取当前列表组 */
void player_handle_list_select_key(key_event_t evt);           /* 处理列表选择模式的按键 */

#endif
