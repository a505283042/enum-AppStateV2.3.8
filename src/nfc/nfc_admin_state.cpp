#include "nfc/nfc_admin_state.h"  /* 包含NFC管理状态模块 */
#include <Arduino.h>                 /* 包含Arduino核心库 */

/* 进入NFC管理状态 - 显示进入NFC管理界面的消息 */
void nfc_admin_state_enter(void)
{
    Serial.println("[NFC_ADMIN] enter");  /* 输出进入NFC管理状态的日志 */
}

/* 退出NFC管理状态 - 显示退出NFC管理界面的消息 */
void nfc_admin_state_exit(void)
{
    Serial.println("[NFC_ADMIN] exit");   /* 输出退出NFC管理状态的日志 */
}

/* 更新NFC管理状态 - 处理NFC卡片检测和绑定信息写入 */
void nfc_admin_state_update(void)
{
    // 以后：检测卡片 / 写入绑定信息
    // TODO: 实现NFC卡片检测和绑定功能
}

/* 运行NFC管理状态 - 执行NFC管理状态的主循环 */
void nfc_admin_state_run(void)
{
    nfc_admin_state_update();  /* 更新NFC管理状态 */
}
