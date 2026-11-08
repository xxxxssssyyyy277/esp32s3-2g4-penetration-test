#ifndef __LCD_DATA_H
#define __LCD_DATA_H

#include <stdint.h>

/**
  ******************************************************************************
  * 文件名称：LCD_Data.h
  * 描    述：LCD字模与图像数据头文件
  *           代码编写风格仿照江协科技OLED驱动程序的OLED_Data.h
  ******************************************************************************
  */

/*字符集定义*********************/

/*以下两个宏定义只可解除其中一个的注释*/
/*且需要和代码编辑器的编码设置保持一致*/
/*VS Code和ESP-IDF默认使用UTF8编码，Keil默认使用GB2312编码*/
#define LCD_CHARSET_UTF8			//定义字符集为UTF8
//#define LCD_CHARSET_GB2312		//定义字符集为GB2312

/*字模数据存储格式说明*********************/

/*纵向8点，高位在下，先从左到右，再从上到下*/
/*每一个Bit对应一个像素点*/
/*
  *      B0 B0                  B0 B0
  *      B1 B1                  B1 B1
  *      B2 B2                  B2 B2
  *      B3 B3  ------------->  B3 B3 --
  *      B4 B4                  B4 B4  |
  *      B5 B5                  B5 B5  |
  *      B6 B6                  B6 B6  |
  *      B7 B7                  B7 B7  |
  *                                    |
  *  -----------------------------------
  *  |
  *  |   B0 B0                  B0 B0
  *  |   B1 B1                  B1 B1
  *  |   B2 B2                  B2 B2
  *  --> B3 B3  ------------->  B3 B3
  *      B4 B4                  B4 B4
  *      B5 B5                  B5 B5
  *      B6 B6                  B6 B6
  *      B7 B7                  B7 B7
  */

/*字模参数单元*/
typedef struct
{
#ifdef LCD_CHARSET_UTF8
	char Index[5];				//字符索引，UTF8编码下最长占用4字节
#endif
#ifdef LCD_CHARSET_GB2312
	char Index[3];				//字符索引，GB2312编码下最长占用2字节
#endif
	uint8_t Data[32];			//字模数据
} ChineseCell_t;

/*ASCII字模数据声明*********************/

/*宽8像素，高16像素*/
extern const uint8_t LCD_F8x16[][16];
/*宽6像素，高8像素*/
extern const uint8_t LCD_F6x8[][6];

/*汉字字模数据声明*********************/

/*宽16像素，高16像素*/
extern const ChineseCell_t LCD_CF16x16[];

/*图像数据声明*********************/

/*图像数据为RGB565格式，每个像素占2字节，先低字节后高字节，先从左到右，再从上到下*/
/*宽16像素，高16像素*/
extern const uint8_t LCD_Image_Diode[];
/*宽32像素，高32像素*/
extern const uint8_t LCD_Image_Color[];

#endif
