/* 存储系统(SD/文件系统)模块头文件 */
#ifndef STORAGE_H
#define STORAGE_H

#include <stdint.h>   /* 包含标准整数类型定义 */
#include <stdbool.h>  /* 包含布尔类型定义 */

/* =========================================================
 * 存储系统(SD/文件系统)
 * ========================================================= */

/* 存储系统初始化函数 */
bool storage_init(void);            /* 初始化存储系统 */

/* 文件/文件夹访问函数 */
bool storage_is_ready(void);        /* 检查存储是否就绪 */
bool storage_dir_exists(const char* path);  /* 检查目录是否存在 */

/* 播放器相关函数 */
bool storage_open_album(const char* folder);  /* 打开专辑文件夹 */

/* 管理/调试函数 */
void storage_print_info(void);      /* 打印存储信息 */

/* 调试用：列出根目录 */
void storage_list_root(void);       /* 列出根目录内容 */

#endif // STORAGE_H