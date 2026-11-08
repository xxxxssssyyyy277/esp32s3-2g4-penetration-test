#ifndef __LCD_H
#define __LCD_H

#include <stdint.h>
#include "driver/gpio.h"
#include "driver/ledc.h"

/**
  ******************************************************************************
  * 文件名称：LCD.h
  * 描    述：1.54寸ST7789 TFT-LCD（240*240）驱动函数头文件
  *           驱动源码取自已验证的 2.4 寸（240*320）工程，只改分辨率/偏移/引脚
  *           驱动方式：软件模拟SPI（时序引脚可任意指定）
  *           代码编写风格仿照江协科技OLED驱动程序，采用显存数组+更新函数机制
  *           背光亮度：使用LEDC硬件PWM波调节，可任意设置0~100的亮度
  * 硬件连接：SCL接GPIO39  SDA接GPIO40  RES接GPIO41  DC接GPIO42
  *           CS接GPIO2    BL接GPIO1（背光，LEDC 通道 0）
  ******************************************************************************
  */

/*引脚配置*********************/

/*SCL时钟线，接屏幕的SCL/CLK引脚，如引脚有变动，只需修改此处的宏定义*/
#define LCD_SCL_PIN			GPIO_NUM_39
/*SDA数据线，接屏幕的SDA/MOSI引脚*/
#define LCD_SDA_PIN			GPIO_NUM_40
/*RES复位线，接屏幕的RES/RST引脚*/
#define LCD_RES_PIN			GPIO_NUM_41
/*DC数据/命令选择线，接屏幕的DC引脚*/
#define LCD_DC_PIN			GPIO_NUM_42
/*CS片选线，接屏幕的CS引脚*/
#define LCD_CS_PIN			GPIO_NUM_2
/*BL背光线，接屏幕的BLK/LED引脚，高电平点亮，使用PWM波驱动以调节亮度*/
#define LCD_BL_PIN			GPIO_NUM_1

/*背光PWM配置*********************/

/*背光PWM使用的LEDC通道和定时器，若与程序中其他功能冲突，可修改为其他空闲通道*/
#define LCD_BL_LEDC_MODE		LEDC_LOW_SPEED_MODE	//LEDC速度模式，使用低速模式（所有芯片都支持）
#define LCD_BL_LEDC_TIMER		LEDC_TIMER_0		//LEDC定时器编号，范围：0~3
#define LCD_BL_LEDC_CHANNEL		LEDC_CHANNEL_0		//LEDC通道编号，范围：0~7

/*背光PWM频率，单位Hz，取值1000~10000比较合适*/
/*频率太低背光会有可见闪烁，频率太高则可能引起背光升压电路出现噪声*/
#define LCD_BL_PWM_FREQ			5000

/*背光PWM占空比分辨率，8位对应占空比0~255，10位对应0~1023，位数越高亮度分级越细腻*/
/*分辨率越高，在同样主频下能达到的最高PWM频率越低，本驱动使用8位足够*/
#define LCD_BL_PWM_RES			LEDC_TIMER_8_BIT

/*背光默认亮度，范围：0~100，LCD_Init函数初始化完成后会设置为该亮度*/
#define LCD_BL_BRIGHTNESS       60

/*屏幕参数配置*********************/

/*屏幕横向像素数（宽），ST7789最大为240*/
#define LCD_WIDTH			240
/*屏幕纵向像素数（高），1.54寸面板为240*/
#define LCD_HEIGHT            240

/*显示区域偏移，若屏幕边缘出现杂纹，可适当调整此值裁剪屏幕边缘*/
/*240*240 面板在 240*320 显存里可能居中：按 0 -> 80 -> 40 依次试 LCD_Y_OFFSET*/
/*LCD_X_OFFSET 依次试 0 -> 80*/
#define LCD_X_OFFSET		0
#define LCD_Y_OFFSET		0

/*颜色定义*********************/

/*由R、G、B三个颜色分量（范围0~255）计算出RGB565的颜色值*/
/* Panel orientation and inversion: configurable, documented in README. */
#define LCD_MADCTL              0x00
#define LCD_INVERSION_ON        1

#define LCD_RGB565(R, G, B)	((uint16_t)((((R) & 0xF8) << 8) | (((G) & 0xFC) << 3) | (((B) & 0xF8) >> 3)))

#define LCD_BLACK			0x0000		//黑色
#define LCD_WHITE			0xFFFF		//白色
#define LCD_RED				0xF800		//红色
#define LCD_GREEN			0x07E0		//绿色
#define LCD_BLUE			0x001F		//蓝色
#define LCD_YELLOW			0xFFE0		//黄色
#define LCD_CYAN			0x07FF		//青色
#define LCD_MAGENTA			0xF81F		//品红色
#define LCD_PURPLE			0x780F		//紫色
#define LCD_ORANGE			0xFD20		//橙色
#define LCD_BROWN			0xBC40		//棕色
#define LCD_GRAY			0x8410		//灰色
#define LCD_DARKGREEN		0x03E0		//深绿色
#define LCD_GRAYBLUE		0x5458		//灰蓝色

/*FontSize参数取值*********************/

/*此参数值不仅用于判断，而且用于计算横向字符偏移，此值代表了每个字符的宽度*/
#define LCD_8X16			8			//宽8像素，高16像素
#define LCD_6X8				6			//宽6像素，高8像素

/*IsFilled参数数值*********************/

#define LCD_UNFILLED		0			//不填充
#define LCD_FILLED			1			//填充

/*函数声明*********************/

/*初始化函数*/
void LCD_Init(void);

/*更新函数*/
void LCD_Update(void);
void LCD_UpdateArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height);

/*显存控制函数*/
void LCD_Clear(uint16_t Color);
void LCD_ClearArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint16_t Color);
void LCD_Reverse(void);
void LCD_ReverseArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height);

/*显示函数*/
void LCD_ShowImage1Bit(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, const uint8_t *Image, uint16_t Color, uint16_t BackColor);
void LCD_ShowImage(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, const uint8_t *Image);
void LCD_ShowImageRLE(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, const uint8_t *RLE);
void LCD_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_ShowString(int16_t X, int16_t Y, const char *String, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize, uint16_t Color, uint16_t BackColor);
void LCD_Printf(int16_t X, int16_t Y, uint8_t FontSize, uint16_t Color, uint16_t BackColor, const char *format, ...);

/*画图函数*/
void LCD_DrawPoint(int16_t X, int16_t Y, uint16_t Color);
uint16_t LCD_GetPoint(int16_t X, int16_t Y);
void LCD_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, uint16_t Color);
void LCD_DrawRectangle(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint16_t Color, uint8_t IsFilled);
void LCD_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint16_t Color, uint8_t IsFilled);
void LCD_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint16_t Color, uint8_t IsFilled);
void LCD_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint16_t Color, uint8_t IsFilled);
void LCD_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint16_t Color, uint8_t IsFilled);

/*背光控制函数*/
void LCD_BackLight_Init(void);
void LCD_SetBacklight(uint8_t On);
void LCD_SetBrightness(uint8_t Brightness);
uint8_t LCD_GetBrightness(void);
void LCD_BrightnessFade(uint8_t Brightness, uint32_t TimeMs);

#endif
