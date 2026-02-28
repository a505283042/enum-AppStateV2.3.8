/* 应用状态管理模块头文件 */
#ifndef APP_STATE_H
#define APP_STATE_H

/* 定义应用程序的各种状态 */
typedef enum {
    STATE_BOOT = 0,      /* 启动状态 */
    STATE_PLAYER,        /* 音乐播放器状态 */
    STATE_NFC_ADMIN,     /* NFC管理状态 */
    STATE_RADIO,         /* 收音机状态 */
} app_state_t;

/* 全局应用状态变量 */
extern app_state_t g_app_state;

/* 应用状态管理函数声明 */
void app_state_init(void);    /* 初始化应用状态 */
void app_state_update(void);  /* 更新应用状态 */

#endif
