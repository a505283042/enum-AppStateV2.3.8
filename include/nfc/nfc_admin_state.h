/* NFC管理状态模块头文件 */
#ifndef NFC_ADMIN_STATE_H
#define NFC_ADMIN_STATE_H

#include <stdint.h>  /* 包含标准整数类型定义 */

/* =========================================================
 * NFC管理状态模块
 * 用于绑定NFC标签到专辑/文件夹
 * ========================================================= */

/* NFC管理状态处理函数声明 */
void nfc_admin_state_enter(void);  /* 进入NFC管理状态 */
void nfc_admin_state_exit(void);   /* 退出NFC管理状态 */
void nfc_admin_state_update(void); /* 更新NFC管理状态 */
void nfc_admin_state_run(void);    /* 运行NFC管理状态 */

#endif // NFC_ADMIN_STATE_H
