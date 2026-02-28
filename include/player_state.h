/* 播放器状态管理模块头文件 */
#ifndef PLAYER_STATE_H
#define PLAYER_STATE_H

/* 播放器状态处理函数声明 */
void player_state_run(void);  /* 运行播放器状态处理函数 */
void player_next_track();
void player_toggle_random();  /* 切换随机播放模式 */
void player_prev_track();
void player_toggle_play();
void player_volume_step(int delta);

#endif
