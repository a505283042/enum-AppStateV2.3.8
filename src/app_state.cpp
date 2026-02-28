#include "app_state.h"         /* 包含应用状态管理模块 */
#include "boot_state.h"         /* 包含启动状态模块 */
#include "player_state.h"       /* 包含播放器状态模块 */
#include "nfc/nfc_admin_state.h" /* 包含NFC管理状态模块 */
#include "keys/keys.h"                /* 包含按键处理模块 */
#include "app_flags.h"
#include "ui/ui.h"

volatile bool g_rescan_done = false;
volatile bool g_rescanning = false;
volatile bool g_random_play = false;
volatile play_mode_t g_play_mode = PLAY_MODE_ALL_SEQ;  // 播放模式

/* 全局应用状态变量，初始值为启动状态 */
app_state_t g_app_state = STATE_BOOT;

/* 初始化应用状态为启动状态 */
void app_state_init(void)
{
    g_app_state = STATE_BOOT;
    keys_init(); /* 初始化按键处理模块 */
}

/* 根据当前应用状态执行相应的状态处理函数 */
void app_state_update(void)
{
    // 按键处理也需要高频调用，确保响应及时
    keys_update();

    switch (g_app_state) {
        case STATE_BOOT:        /* 如果是启动状态，则运行启动状态处理函数 */
            boot_state_run();
            break;

        case STATE_PLAYER:      /* 如果是播放器状态，则运行播放器状态处理函数 */
            player_state_run();
            break;

        case STATE_NFC_ADMIN:   /* 如果是NFC管理状态，则运行NFC管理状态处理函数 */
            nfc_admin_state_run();
            break;

        default:                /* 默认情况下不执行任何操作 */
            break;
    }
}
