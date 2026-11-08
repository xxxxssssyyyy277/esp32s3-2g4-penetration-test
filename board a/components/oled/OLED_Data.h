#ifndef __OLED_DATA_H
#define __OLED_DATA_H

#include <stdint.h>

/* 原驱动用 GB2312 字库，汉字按 GB2312 字节索引；本工程屏幕内容全为 ASCII，
 * 汉字字模与查询分支已删除，因此不需要字符集宏。
 * 若后续要加汉字，请用 UTF-8 字模并在这里定义 OLED_CHARSET_UTF8。 */

/* 8x16 ASCII 字模，索引 = 字符 - ' '，共 95 个字符（0x20~0x7E） */
extern const uint8_t OLED_F8x16[][16];

#endif /* __OLED_DATA_H */
