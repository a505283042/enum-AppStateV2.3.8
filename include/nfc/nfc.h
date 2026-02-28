/* NFC接口模块头文件 */
#ifndef NFC_H
#define NFC_H

#include <stdint.h>  /* 包含标准整数类型定义 */

/* =========================================================
 * NFC接口
 * 主要NFC功能：初始化和轮询
 * ========================================================= */

/* NFC模块初始化和轮询函数 */
void nfc_init(void);   /* 初始化NFC模块 */
void nfc_poll(void);   /* 轮询NFC模块 */

#endif // NFC_H