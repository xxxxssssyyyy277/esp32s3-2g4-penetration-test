#include "LCD.h"
#include "LCD_Data.h"
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"

/**
  ******************************************************************************
  * 文件名称：LCD.c
  * 描    述：1.54寸ST7789 TFT-LCD（240*240）驱动函数，软件模拟SPI通信
  *           驱动源码取自已验证的 2.4 寸（240*320）工程，
  *           本项目只把 LCD_HEIGHT 改为 240、偏移改为 0、换成本板引脚
  *           代码编写风格仿照江协科技OLED驱动程序
  *           采用显存数组+更新函数的机制，所有显示函数只读写显存数组
  *           随后调用LCD_Update函数或LCD_UpdateArea函数，才会将显存数组的
  *           数据发送到LCD屏幕，进行显示
  ******************************************************************************
  */

/*全局变量*********************/

/*LCD显存数组，每个像素占用2字节（RGB565格式），共240*240*2=115200字节*/
/*所有显示函数只读写此数组，调用LCD_Update函数或LCD_UpdateArea函数才会真正显示到屏幕*/
uint16_t LCD_DisplayBuf[LCD_HEIGHT][LCD_WIDTH];

/*背光当前亮度，范围：0~100，由LCD_SetBrightness函数和LCD_BrightnessFade函数更新*/
static uint8_t LCD_BackLightBrightness = 0;

/*背光PWM初始化标志，0表示未初始化，1表示已初始化*/
static uint8_t LCD_BackLightInited = 0;


/*引脚配置*********************/

/**
  * 函    数：GPIO输出寄存器直写
  * 参    数：Pin 指定引脚编号，范围：0~48
  * 参    数：Level 要写入的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：此函数为引脚操作的最底层函数，仅供本文件内部使用
  *           使用寄存器直写的方式，速度比gpio_set_level函数更快
  *           若希望代码更易读，也可将函数体替换为gpio_set_level(Pin, Level)
  */
void LCD_GPIO_Set(uint32_t Pin, uint32_t Level)
{
	if (Pin < 32)
	{
		/*引脚编号小于32，使用GPIO_OUT_W1TS_REG和GPIO_OUT_W1TC_REG寄存器*/
		/*W1TS寄存器写1的位，对应的引脚输出高电平，写0的位无影响*/
		/*W1TC寄存器写1的位，对应的引脚输出低电平，写0的位无影响*/
		REG_WRITE(Level ? GPIO_OUT_W1TS_REG : GPIO_OUT_W1TC_REG, 1UL << Pin);
	}
	else
	{
		/*引脚编号大于等于32，使用GPIO_OUT1_W1TS_REG和GPIO_OUT1_W1TC_REG寄存器*/
		/*此处引脚编号需要减去32，对应高位引脚的位序号*/
		REG_WRITE(Level ? GPIO_OUT1_W1TS_REG : GPIO_OUT1_W1TC_REG, 1UL << (Pin - 32));
	}
}

/**
  * 函    数：LCD写SCL高低电平
  * 参    数：BitValue 要写入SCL的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：当上层函数需要写SCL时，此函数会被调用
  *           用户需要根据参数传入的值，将SCL置为高电平或者低电平
  *           当参数传入0时，置SCL为低电平，当参数传入1时，置SCL为高电平
  */
void LCD_W_SCL(uint8_t BitValue)
{
	/*根据BitValue的值，将SCL置高电平或者低电平*/
	LCD_GPIO_Set(LCD_SCL_PIN, BitValue);
}

/**
  * 函    数：LCD写SDA高低电平
  * 参    数：BitValue 要写入SDA的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：当上层函数需要写SDA时，此函数会被调用
  *           用户需要根据参数传入的值，将SDA置为高电平或者低电平
  *           当参数传入0时，置SDA为低电平，当参数传入1时，置SDA为高电平
  */
void LCD_W_SDA(uint8_t BitValue)
{
	/*根据BitValue的值，将SDA置高电平或者低电平*/
	LCD_GPIO_Set(LCD_SDA_PIN, BitValue);
}

/**
  * 函    数：LCD写RES高低电平
  * 参    数：BitValue 要写入RES的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：当上层函数需要写RES时，此函数会被调用
  *           用户需要根据参数传入的值，将RES置为高电平或者低电平
  *           RES为低电平时，LCD芯片复位
  */
void LCD_W_RES(uint8_t BitValue)
{
	/*根据BitValue的值，将RES置高电平或者低电平*/
	LCD_GPIO_Set(LCD_RES_PIN, BitValue);
}

/**
  * 函    数：LCD写DC高低电平
  * 参    数：BitValue 要写入DC的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：当上层函数需要写DC时，此函数会被调用
  *           用户需要根据参数传入的值，将DC置为高电平或者低电平
  *           DC为低电平时，表示即将发送的是命令
  *           DC为高电平时，表示即将发送的是数据
  */
void LCD_W_DC(uint8_t BitValue)
{
	/*根据BitValue的值，将DC置高电平或者低电平*/
	LCD_GPIO_Set(LCD_DC_PIN, BitValue);
}

/**
  * 函    数：LCD写CS高低电平
  * 参    数：BitValue 要写入CS的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：当上层函数需要写CS时，此函数会被调用
  *           用户需要根据参数传入的值，将CS置为高电平或者低电平
  *           CS为低电平时，表示选中LCD芯片，可以开始通信
  */
void LCD_W_CS(uint8_t BitValue)
{
	/*根据BitValue的值，将CS置高电平或者低电平*/
	LCD_GPIO_Set(LCD_CS_PIN, BitValue);
}

/**
  * 函    数：LCD写BL高低电平
  * 参    数：BitValue 要写入BL的电平值，范围：0/1
  * 返 回 值：无
  * 说    明：当上层函数需要写BL时，此函数会被调用
  *           用户需要根据参数传入的值，将BL置为高电平或者低电平
  *           BL为高电平时，LCD背光点亮，为低电平时，LCD背光熄灭
  */
void LCD_W_BL(uint8_t BitValue)
{
	/*根据BitValue的值，将BL置高电平或者低电平*/
	LCD_GPIO_Set(LCD_BL_PIN, BitValue);
}

/**
  * 函    数：LCD引脚初始化
  * 参    数：无
  * 返 回 值：无
  * 说    明：当上层函数需要初始化时，此函数会被调用
  *           用户需要将SCL、SDA、RES、DC、CS和BL引脚初始化为输出模式
  */
void LCD_GPIO_Init(void)
{
	/*引脚配置结构体，将6个引脚全部配置为普通输出模式*/
	gpio_config_t GPIO_InitStructure = {0};
	GPIO_InitStructure.pin_bit_mask = (1ULL << LCD_SCL_PIN) | (1ULL << LCD_SDA_PIN) |
									  (1ULL << LCD_RES_PIN) | (1ULL << LCD_DC_PIN)  |
									  (1ULL << LCD_CS_PIN)  | (1ULL << LCD_BL_PIN);
	GPIO_InitStructure.mode = GPIO_MODE_OUTPUT;
	GPIO_InitStructure.pull_up_en = GPIO_PULLUP_DISABLE;
	GPIO_InitStructure.pull_down_en = GPIO_PULLDOWN_DISABLE;
	GPIO_InitStructure.intr_type = GPIO_INTR_DISABLE;
	gpio_config(&GPIO_InitStructure);
	
	/*置引脚默认电平*/
	LCD_W_SCL(1);		//SCL空闲时为高电平
	LCD_W_SDA(1);		//SDA空闲时为高电平
	LCD_W_RES(1);		//RES复位线不动作
	LCD_W_DC(1);		//DC默认数据模式
	LCD_W_CS(1);		//CS片选不选中
	LCD_W_BL(0);		//BL背光先关闭，待背光PWM初始化后再设置亮度
	
	/*在初始化前，加入适量延时，待LCD供电稳定*/
	vTaskDelay(pdMS_TO_TICKS(50));
}

/*********************引脚配置*/

/*********************通信协议*********************/

/**
  * 函    数：SPI发送一个字节
  * 参    数：Byte 要发送的一个字节数据，范围：0x00~0xFF
  * 返 回 值：无
  * 说    明：软件模拟SPI通信，模式0（SCL空闲为低电平，上升沿时从机读取SDA）
  *           数据高位先发，先发送Bit7，最后发送Bit0
  */
void LCD_SPI_SendByte(uint8_t Byte)
{
	uint8_t i;
	
	/*循环8次，主机依次发送数据的每一位*/
	for (i = 0; i < 8; i++)
	{
		/*使用掩码的方式取出Byte的指定一位数据并写入到SDA线*/
		/*两个!的作用是，让所有非零的值变为1*/
		LCD_W_SDA(!!(Byte & (0x80 >> i)));
		LCD_W_SCL(1);				//拉高SCL，从机在SCL上升沿读取SDA
		LCD_W_SCL(0);				//拉低SCL，主机开始发送下一位数据
	}
}

/**
  * 函    数：SPI连续发送多个字节
  * 参    数：Data 要发送数据的起始地址
  * 参    数：Count 要发送数据的数量
  * 返 回 值：无
  * 说    明：此函数与LCD_SPI_SendByte函数的功能相同
  *           但此函数减少了循环外的函数调用开销，发送速度更快
  */
void LCD_SPI_SendBytes(const uint8_t *Data, uint32_t Count)
{
	uint32_t i;
	uint8_t j;
	uint8_t Byte;
	
	/*循环Count次，进行连续的数据发送*/
	for (i = 0; i < Count; i++)
	{
		Byte = Data[i];
		
		/*循环8次，主机依次发送数据的每一位*/
		for (j = 0; j < 8; j++)
		{
			LCD_W_SDA(!!(Byte & (0x80 >> j)));
			LCD_W_SCL(1);			//拉高SCL，从机在SCL上升沿读取SDA
			LCD_W_SCL(0);			//拉低SCL，主机开始发送下一位数据
		}
	}
}

/**
  * 函    数：LCD写命令
  * 参    数：Command 要写入的命令值，范围：0x00~0xFF
  * 返 回 值：无
  */
void LCD_WriteCommand(uint8_t Command)
{
	LCD_W_CS(0);					//拉低CS，开始通信
	LCD_W_DC(0);					//拉低DC，表示即将发送命令
	LCD_SPI_SendByte(Command);		//写入指定命令
	LCD_W_CS(1);					//拉高CS，结束通信
}

/**
  * 函    数：LCD写数据
  * 参    数：Data 要写入数据的起始地址
  * 参    数：Count 要写入数据的数量
  * 返 回 值：无
  */
void LCD_WriteData(const uint8_t *Data, uint32_t Count)
{
	uint32_t i;
	
	LCD_W_CS(0);					//拉低CS，开始通信
	LCD_W_DC(1);					//拉高DC，表示即将发送数据
	/*循环Count次，进行连续的数据写入*/
	for (i = 0; i < Count; i++)
	{
		LCD_SPI_SendByte(Data[i]);	//依次发送Data的每一个数据
	}
	LCD_W_CS(1);					//拉高CS，结束通信
}

/*********************通信协议*/

/*********************硬件配置*********************/

/*用户可修改的屏幕配置*/

/*屏幕扫描方向配置，若显示方向不符合需求，可修改 LCD.h 中的 LCD_MADCTL*/
/*MX=0x40（左右翻转），MY=0x80（上下翻转），MV=0x20（交换XY）*/
/*RGB=0x00（RGB顺序），BGR=0x08（BGR顺序，若红蓝颜色互换可修改此位）*/

/*屏幕是否开启反色显示*/
/*绝大多数IPS屏需要开启反色显示（值为1）*/
/*若显示的颜色与预期完全相反，可将 LCD.h 中的 LCD_INVERSION_ON 改为 0*/
#define LCD_INVERSION		LCD_INVERSION_ON

/**
  * 函    数：LCD初始化
  * 参    数：无
  * 返 回 值：无
  * 说    明：调用此函数后，LCD即可正常显示
  *           初始化内容包括：引脚初始化、硬件复位、ST7789寄存器配置、
  *           清屏和背光PWM初始化（初始亮度由LCD_BL_BRIGHTNESS宏指定）
  */
void LCD_Init(void)
{
	uint8_t Data[16];
	
	/*引脚初始化*/
	LCD_GPIO_Init();
	
	/*硬件复位，RES引脚先拉低，再拉高，随后等待复位完成*/
	LCD_W_RES(0);
	vTaskDelay(pdMS_TO_TICKS(10));
	LCD_W_RES(1);
	vTaskDelay(pdMS_TO_TICKS(120));
	
	/*软件复位*/
	LCD_WriteCommand(0x01);
	vTaskDelay(pdMS_TO_TICKS(120));
	
	/*退出睡眠模式*/
	LCD_WriteCommand(0x11);
	vTaskDelay(pdMS_TO_TICKS(120));
	
	/*设置屏幕扫描方向*/
	LCD_WriteCommand(0x36);
	Data[0] = LCD_MADCTL;
	LCD_WriteData(Data, 1);
	
	/*设置像素格式为16位，即RGB565*/
	LCD_WriteCommand(0x3A);
	Data[0] = 0x55;
	LCD_WriteData(Data, 1);
	
	/*设置前后廊（Porch）*/
	LCD_WriteCommand(0xB2);
	Data[0] = 0x0C; Data[1] = 0x0C; Data[2] = 0x00; Data[3] = 0x33; Data[4] = 0x33;
	LCD_WriteData(Data, 5);
	
	/*设置门控控制（Gate Control）*/
	LCD_WriteCommand(0xB7);
	Data[0] = 0x35;
	LCD_WriteData(Data, 1);
	
	/*设置VCOM*/
	LCD_WriteCommand(0xBB);
	Data[0] = 0x19;
	LCD_WriteData(Data, 1);
	
	/*设置LCM控制*/
	LCD_WriteCommand(0xC0);
	Data[0] = 0x2C;
	LCD_WriteData(Data, 1);
	
	/*设置VDV和VRH使能*/
	LCD_WriteCommand(0xC2);
	Data[0] = 0x01;
	LCD_WriteData(Data, 1);
	
	/*设置VRH*/
	LCD_WriteCommand(0xC3);
	Data[0] = 0x12;
	LCD_WriteData(Data, 1);
	
	/*设置VDV*/
	LCD_WriteCommand(0xC4);
	Data[0] = 0x20;
	LCD_WriteData(Data, 1);
	
	/*设置刷新率，0x0F为60Hz*/
	LCD_WriteCommand(0xC6);
	Data[0] = 0x0F;
	LCD_WriteData(Data, 1);
	
	/*设置电源控制*/
	LCD_WriteCommand(0xD0);
	Data[0] = 0xA4; Data[1] = 0xA1;
	LCD_WriteData(Data, 2);
	
	/*设置正极性gamma校正*/
	LCD_WriteCommand(0xE0);
	Data[0] = 0xD0; Data[1] = 0x04; Data[2] = 0x0D; Data[3] = 0x11;
	Data[4] = 0x13; Data[5] = 0x2B; Data[6] = 0x3F; Data[7] = 0x54;
	Data[8] = 0x4C; Data[9] = 0x18; Data[10] = 0x0D; Data[11] = 0x0B;
	Data[12] = 0x1F; Data[13] = 0x23;
	LCD_WriteData(Data, 14);
	
	/*设置负极性gamma校正*/
	LCD_WriteCommand(0xE1);
	Data[0] = 0xD0; Data[1] = 0x04; Data[2] = 0x0C; Data[3] = 0x11;
	Data[4] = 0x13; Data[5] = 0x2C; Data[6] = 0x3F; Data[7] = 0x44;
	Data[8] = 0x51; Data[9] = 0x2F; Data[10] = 0x1F; Data[11] = 0x1F;
	Data[12] = 0x20; Data[13] = 0x23;
	LCD_WriteData(Data, 14);
	
	/*设置反色显示，由LCD_INVERSION的值决定*/
	if (LCD_INVERSION)
	{
		LCD_WriteCommand(0x21);		//开启反色显示
	}
	else
	{
		LCD_WriteCommand(0x20);		//关闭反色显示
	}
	
	/*开启显示*/
	LCD_WriteCommand(0x29);
	vTaskDelay(pdMS_TO_TICKS(10));
	
	/*清空显存数组*/
	LCD_Clear(LCD_BLACK);
	/*更新显示，清屏，防止初始化后未显示内容时花屏*/
	LCD_Update();
	
	/*初始化背光PWM，并将背光亮度设置为默认值*/
	LCD_BackLight_Init();
	LCD_SetBrightness(LCD_BL_BRIGHTNESS);
}

/**
  * 函    数：LCD设置显示地址
  * 参    数：X1 Y1 指定区域左上角的横纵坐标
  * 参    数：X2 Y2 指定区域右下角的横纵坐标
  * 返 回 值：无
  * 说    明：调用此函数后，后续写入的像素数据会依次填充到指定区域
  *           此函数仅供本文件内部使用
  */
void LCD_SetAddress(uint16_t X1, uint16_t Y1, uint16_t X2, uint16_t Y2)
{
	uint8_t Data[4];
	
	/*加上显示区域偏移，用于兼容不同尺寸的屏幕*/
	X1 += LCD_X_OFFSET;
	X2 += LCD_X_OFFSET;
	Y1 += LCD_Y_OFFSET;
	Y2 += LCD_Y_OFFSET;
	
	/*设置列地址（X轴）*/
	LCD_WriteCommand(0x2A);
	Data[0] = (uint8_t)(X1 >> 8); Data[1] = (uint8_t)X1;
	Data[2] = (uint8_t)(X2 >> 8); Data[3] = (uint8_t)X2;
	LCD_WriteData(Data, 4);
	
	/*设置行地址（Y轴）*/
	LCD_WriteCommand(0x2B);
	Data[0] = (uint8_t)(Y1 >> 8); Data[1] = (uint8_t)Y1;
	Data[2] = (uint8_t)(Y2 >> 8); Data[3] = (uint8_t)Y2;
	LCD_WriteData(Data, 4);
	
	/*写显存指令，后续写入的数据即为像素数据*/
	LCD_WriteCommand(0x2C);
}

/**
  * 函    数：LCD背光PWM初始化
  * 参    数：无
  * 返 回 值：无
  * 说    明：初始化LEDC的定时器和通道，将PWM波输出到BL引脚，用于调节背光亮度
  *           PWM波的频率由LCD_BL_PWM_FREQ宏指定，占空比分辨率由LCD_BL_PWM_RES宏指定
  *           此函数由LCD_Init函数调用，用户也可自行调用（重复调用无副作用）
  */
void LCD_BackLight_Init(void)
{
	ledc_timer_config_t BackLightTimerStructure = {0};
	ledc_channel_config_t BackLightChannelStructure = {0};
	
	/*配置LEDC定时器，用于产生指定频率的PWM波*/
	BackLightTimerStructure.speed_mode = LCD_BL_LEDC_MODE;			//LEDC速度模式
	BackLightTimerStructure.timer_num = LCD_BL_LEDC_TIMER;			//LEDC定时器编号
	BackLightTimerStructure.duty_resolution = LCD_BL_PWM_RES;		//占空比分辨率
	BackLightTimerStructure.freq_hz = LCD_BL_PWM_FREQ;				//PWM波频率
	BackLightTimerStructure.clk_cfg = LEDC_AUTO_CLK;				//自动选择时钟源
	ledc_timer_config(&BackLightTimerStructure);
	
	/*配置LEDC通道，将定时器产生的PWM波输出到BL引脚*/
	BackLightChannelStructure.speed_mode = LCD_BL_LEDC_MODE;		//LEDC速度模式
	BackLightChannelStructure.channel = LCD_BL_LEDC_CHANNEL;		//LEDC通道编号
	BackLightChannelStructure.timer_sel = LCD_BL_LEDC_TIMER;		//通道使用上方的定时器
	BackLightChannelStructure.gpio_num = LCD_BL_PIN;				//PWM波输出引脚
	BackLightChannelStructure.duty = 0;								//占空比先设为0，即背光先熄灭
	BackLightChannelStructure.hpoint = 0;							//高电平起始点
	BackLightChannelStructure.intr_type = LEDC_INTR_DISABLE;		//不使用中断
	ledc_channel_config(&BackLightChannelStructure);
	
	/*安装渐变（Fade）功能，LCD_BrightnessFade函数需要用到，此函数内部会配置LEDC中断*/
	ledc_fade_func_install(0);
	
	/*置背光PWM初始化标志为1，表示已经初始化*/
	LCD_BackLightInited = 1;
}

/**
  * 函    数：LCD背光开关控制
  * 参    数：On 指定背光是否打开，范围：0/1
  * 返 回 值：无
  * 说    明：调用此函数可以随时打开或关闭LCD背光
  *           本质是将背光亮度设置为100%（打开）或0%（关闭）
  *           若需要以任意亮度显示，请使用LCD_SetBrightness函数
  */
void LCD_SetBacklight(uint8_t On)
{
	LCD_SetBrightness(On ? 100 : 0);
}

/**
  * 函    数：LCD设置背光亮度
  * 参    数：Brightness 指定背光亮度，范围：0~100（0为最暗，100为最亮）
  * 返 回 值：无
  * 说    明：此函数通过改变PWM波的占空比来调节背光亮度，亮度越高，占空比越大
  *           传入的亮度超过100时按100处理
  *           若背光PWM还没有初始化，此函数会自动调用LCD_BackLight_Init函数
  */
void LCD_SetBrightness(uint8_t Brightness)
{
	/*若背光PWM未初始化，先初始化*/
	if (LCD_BackLightInited == 0) {LCD_BackLight_Init();}
	
	/*参数限幅，亮度最大为100*/
	if (Brightness > 100) {Brightness = 100;}
	
	/*计算并设置占空比，占空比最大值为2的LCD_BL_PWM_RES次方减1（8位对应255）*/
	ledc_set_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, ((1UL << LCD_BL_PWM_RES) - 1) * Brightness / 100);
	/*更新占空比，使新的占空比立即生效*/
	ledc_update_duty(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL);
	
	/*记录当前亮度，供LCD_GetBrightness函数读取*/
	LCD_BackLightBrightness = Brightness;
}

/**
  * 函    数：LCD获取当前背光亮度
  * 参    数：无
  * 返 回 值：获取到的当前背光亮度，范围：0~100
  * 说    明：返回值由LCD_SetBrightness函数和LCD_BrightnessFade函数记录
  *           若使用LEDC的其他函数直接修改了占空比，此返回值不会自动更新
  */
uint8_t LCD_GetBrightness(void)
{
	return LCD_BackLightBrightness;
}

/**
  * 函    数：LCD背光亮度渐变
  * 参    数：Brightness 指定渐变到的目标亮度，范围：0~100
  * 参    数：TimeMs 指定渐变过程持续的时间，单位：ms，范围：1~65535
  * 返 回 值：无
  * 说    明：此函数让背光亮度在指定时间内平滑地变化到目标亮度，可用于呼吸灯效果
  *           此函数不阻塞，调用后立即返回，渐变过程由LEDC硬件自动完成
  *           若背光PWM还没有初始化，此函数会自动调用LCD_BackLight_Init函数
  */
void LCD_BrightnessFade(uint8_t Brightness, uint32_t TimeMs)
{
	/*若背光PWM未初始化，先初始化*/
	if (LCD_BackLightInited == 0) {LCD_BackLight_Init();}
	
	/*参数限幅，亮度最大为100，渐变时间限制在1~65535ms之间*/
	if (Brightness > 100) {Brightness = 100;}
	if (TimeMs == 0) {TimeMs = 1;}
	if (TimeMs > 65535) {TimeMs = 65535;}
	
	/*设置渐变的目标占空比和渐变时间，并启动渐变，LEDC_FADE_NO_WAIT表示不阻塞等待渐变完成*/
	ledc_set_fade_with_time(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, ((1UL << LCD_BL_PWM_RES) - 1) * Brightness / 100, TimeMs);
	ledc_fade_start(LCD_BL_LEDC_MODE, LCD_BL_LEDC_CHANNEL, LEDC_FADE_NO_WAIT);
	
	/*记录目标亮度，供LCD_GetBrightness函数读取*/
	LCD_BackLightBrightness = Brightness;
}

/*********************硬件配置*/

/*********************工具函数*********************/

/*工具函数仅供内部部分函数使用*/

/**
  * 函    数：次方函数
  * 参    数：X 底数
  * 参    数：Y 指数
  * 返 回 值：等于X的Y次方
  */
uint32_t LCD_Pow(uint32_t X, uint32_t Y)
{
	uint32_t Result = 1;	//结果默认为1
	while (Y --)			//累乘Y次
	{
		Result *= X;		//每次把X累乘到结果上
	}
	return Result;
}

/**
  * 函    数：判断指定点是否在指定多边形内部
  * 参    数：nvert 多边形的顶点数
  * 参    数：vertx verty 包含多边形顶点的x和y坐标的数组
  * 参    数：testx testy 测试点的X和y坐标
  * 返 回 值：指定点是否在指定多边形内部，1：在内部，0：不在内部
  */
uint8_t LCD_pnpoly(uint8_t nvert, int16_t *vertx, int16_t *verty, int16_t testx, int16_t testy)
{
	int16_t i, j, c = 0;
	
	/*此算法由W. Randolph Franklin提出*/
	/*参考链接：https://wrfranklin.org/Research/Short_Notes/pnpoly.html*/
	for (i = 0, j = nvert - 1; i < nvert; j = i ++)
	{
		if (((verty[i] > testy) != (verty[j] > testy)) &&
			(testx < (vertx[j] - vertx[i]) * (testy - verty[i]) / (verty[j] - verty[i]) + vertx[i]))
		{
			c = !c;
		}
	}
	return c;
}

/**
  * 函    数：判断指定点是否在指定角度内部
  * 参    数：X Y 指定点的坐标
  * 参    数：StartAngle EndAngle 起始角度和终止角度，范围：-180~180
  *           水平向右为0度，水平向左为180度或-180度，下方为正数，上方为负数，顺时针旋转
  * 返 回 值：指定点是否在指定角度内部，1：在内部，0：不在内部
  */
uint8_t LCD_IsInAngle(int16_t X, int16_t Y, int16_t StartAngle, int16_t EndAngle)
{
	int16_t PointAngle;
	PointAngle = atan2(Y, X) / 3.14 * 180;	//计算指定点的弧度，并转换为角度表示
	if (StartAngle < EndAngle)				//起始角度小于终止角度的情况
	{
		/*如果指定角度在起始终止角度之间，则判定指定点在指定角度*/
		if (PointAngle >= StartAngle && PointAngle <= EndAngle)
		{
			return 1;
		}
	}
	else									//起始角度大于终止角度的情况
	{
		/*如果指定角度大于起始角度或者小于终止角度，则判定指定点在指定角度*/
		if (PointAngle >= StartAngle || PointAngle <= EndAngle)
		{
			return 1;
		}
	}
	return 0;			//不满足以上条件，则判定指定点不在指定角度
}

/*********************工具函数*/


/*功能函数*********************/

/**
  * 函    数：将LCD显存数组更新到LCD屏幕
  * 参    数：无
  * 返 回 值：无
  * 说    明：所有的显示函数，都只是对LCD显存数组进行读写
  *           随后调用LCD_Update函数或LCD_UpdateArea函数
  *           才会将显存数组的数据发送到LCD硬件，进行显示
  *           故调用显示函数后，要想真正地呈现在屏幕上，还需调用更新函数
  *           软件SPI刷新全屏需要一定的时间，建议尽量使用LCD_UpdateArea函数
  */
void LCD_Update(void)
{
	/*将整个屏幕的区域作为更新区域*/
	LCD_UpdateArea(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

/**
  * 函    数：将LCD显存数组部分更新到LCD屏幕
  * 参    数：X 指定区域左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定区域左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定区域的宽度，范围：0~240
  * 参    数：Height 指定区域的高度，范围：0~320
  * 返 回 值：无
  * 说    明：此函数会更新参数指定的区域，超出屏幕范围的部分不显示
  *           所有的显示函数，都只是对LCD显存数组进行读写
  *           随后调用LCD_Update函数或LCD_UpdateArea函数
  *           才会将显存数组的数据发送到LCD硬件，进行显示
  */
void LCD_UpdateArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
	int16_t i, j;
	uint16_t Color;
	static uint8_t LineData[LCD_WIDTH * 2];		//一行像素的发送缓存，最大240*2=480字节
	
	/*区域宽度或高度为0，则无需更新*/
	if (Width == 0 || Height == 0) {return;}
	
	/*将更新区域裁剪到屏幕范围内，超出屏幕的内容不显示*/
	if (X < 0)									//区域左侧超出屏幕
	{
		if (Width <= -X) {return;}				//区域完全在屏幕外，直接返回
		Width += X;								//宽度减去超出的部分
		X = 0;
	}
	if (Y < 0)									//区域上侧超出屏幕
	{
		if (Height <= -Y) {return;}				//区域完全在屏幕外，直接返回
		Height += Y;							//高度减去超出的部分
		Y = 0;
	}
	if (X >= LCD_WIDTH || Y >= LCD_HEIGHT) {return;}		//区域完全在屏幕外，直接返回
	if (X + Width > LCD_WIDTH) {Width = LCD_WIDTH - X;}		//区域右侧超出屏幕，裁剪宽度
	if (Y + Height > LCD_HEIGHT) {Height = LCD_HEIGHT - Y;}	//区域下侧超出屏幕，裁剪高度
	
	/*设置显示地址，此后写入的像素数据依次填充到指定区域*/
	LCD_SetAddress(X, Y, X + Width - 1, Y + Height - 1);
	
	/*拉低CS、拉高DC，开始连续写入像素数据*/
	LCD_W_CS(0);
	LCD_W_DC(1);
	
	/*遍历指定区域的每一行*/
	for (j = 0; j < Height; j ++)
	{
		/*将显存数组中的一行像素转换到发送缓存*/
		/*LCD要求先发送像素数据的高字节，再发送低字节*/
		for (i = 0; i < Width; i ++)
		{
			Color = LCD_DisplayBuf[Y + j][X + i];
			LineData[i * 2] = (uint8_t)(Color >> 8);
			LineData[i * 2 + 1] = (uint8_t)(Color & 0x00FF);
		}
		/*连续发送一行像素的数据*/
		LCD_SPI_SendBytes(LineData, Width * 2);
	}
	
	/*拉高CS，结束通信*/
	LCD_W_CS(1);
}

/**
  * 函    数：将LCD显存数组全部填充为指定颜色
  * 参    数：Color 指定填充的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_Clear(uint16_t Color)
{
	uint16_t i, j;
	for (j = 0; j < LCD_HEIGHT; j ++)		//遍历240行
	{
		for (i = 0; i < LCD_WIDTH; i ++)	//遍历240列
		{
			LCD_DisplayBuf[j][i] = Color;	//将显存数组数据全部填充为指定颜色
		}
	}
}

/**
  * 函    数：将LCD显存数组部分填充为指定颜色
  * 参    数：X 指定区域左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定区域左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定区域的宽度，范围：0~240
  * 参    数：Height 指定区域的高度，范围：0~320
  * 参    数：Color 指定填充的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ClearArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint16_t Color)
{
	int16_t i, j;
	
	for (j = Y; j < Y + Height; j ++)		//遍历指定行
	{
		for (i = X; i < X + Width; i ++)	//遍历指定列
		{
			if (i >= 0 && i < LCD_WIDTH && j >= 0 && j < LCD_HEIGHT)	//超出屏幕的内容不显示
			{
				LCD_DisplayBuf[j][i] = Color;	//将显存数组指定数据填充为指定颜色
			}
		}
	}
}

/**
  * 函    数：将LCD显存数组全部取反
  * 参    数：无
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_Reverse(void)
{
	uint16_t i, j;
	for (j = 0; j < LCD_HEIGHT; j ++)					//遍历240行
	{
		for (i = 0; i < LCD_WIDTH; i ++)				//遍历240列
		{
			LCD_DisplayBuf[j][i] = ~LCD_DisplayBuf[j][i];	//将显存数组数据全部取反
		}
	}
}

/**
  * 函    数：将LCD显存数组部分取反
  * 参    数：X 指定区域左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定区域左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定区域的宽度，范围：0~240
  * 参    数：Height 指定区域的高度，范围：0~320
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ReverseArea(int16_t X, int16_t Y, uint16_t Width, uint16_t Height)
{
	int16_t i, j;
	
	for (j = Y; j < Y + Height; j ++)		//遍历指定行
	{
		for (i = X; i < X + Width; i ++)	//遍历指定列
		{
			if (i >= 0 && i < LCD_WIDTH && j >= 0 && j < LCD_HEIGHT)	//超出屏幕的内容不显示
			{
				LCD_DisplayBuf[j][i] = ~LCD_DisplayBuf[j][i];	//将显存数组指定数据取反
			}
		}
	}
}

/**
  * 函    数：LCD显示单色图像（1位每像素，字模数据格式）
  * 参    数：X 指定图像左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定图像左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定图像的宽度，范围：0~240
  * 参    数：Height 指定图像的高度，范围：0~320
  * 参    数：Image 指定要显示的图像，数据格式见LCD_Data.h文件的说明
  * 参    数：Color 指定图像中数据为1的像素显示的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定图像中数据为0的像素显示的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：此函数用于显示单色图像，LCD的字模数据都是这种格式
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowImage1Bit(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, const uint8_t *Image, uint16_t Color, uint16_t BackColor)
{
	uint16_t i, j;
	
	/*将图像所在区域填充为背景色*/
	LCD_ClearArea(X, Y, Width, Height, BackColor);
	
	/*遍历指定图像的每一行*/
	for (j = 0; j < Height; j ++)
	{
		/*遍历指定图像的每一列*/
		for (i = 0; i < Width; i ++)
		{
			/*判断图像数据中的指定数据位，为1则在指定位置画点*/
			/*图像数据每8行数据占用Height/8个字节，第j行对应字节中的第j%8位*/
			if (Image[(j / 8) * Width + i] & (0x01 << (j % 8)))
			{
				LCD_DrawPoint(X + i, Y + j, Color);
			}
		}
	}
}

/**
  * 函    数：LCD显示一个字符
  * 参    数：X 指定字符左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定字符左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Char 指定要显示的字符，范围：ASCII码可见字符
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定字符的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定字符背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	/*排除非ASCII可见字符，防止数组越界*/
	if (Char < ' ' || Char > '~') {return;}
	
	if (FontSize == LCD_8X16)		//字体为宽8像素，高16像素
	{
		/*将ASCII字模库LCD_F8x16的指定数据以8*16的图像格式显示*/
		LCD_ShowImage1Bit(X, Y, 8, 16, LCD_F8x16[Char - ' '], Color, BackColor);
	}
	else if (FontSize == LCD_6X8)	//字体为宽6像素，高8像素
	{
		/*将ASCII字模库LCD_F6x8的指定数据以6*8的图像格式显示*/
		LCD_ShowImage1Bit(X, Y, 6, 8, LCD_F6x8[Char - ' '], Color, BackColor);
	}
}

/**
  * 函    数：LCD显示字符串（支持ASCII码和中文混合写入）
  * 参    数：X 指定字符串左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定字符串左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：String 指定要显示的字符串，范围：ASCII码可见字符或中文字符组成的字符串
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定字符串的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定字符串背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：显示的中文字符需要在LCD_Data.c里的LCD_CF16x16数组定义
  *           未找到指定中文字符时，会显示默认图形（一个方框，内部一个问号）
  *           当字体大小为LCD_8X16时，中文字符以16*16点阵正常显示
  *           当字体大小为LCD_6X8时，中文字符以6*8点阵显示'?'
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowString(int16_t X, int16_t Y, const char *String, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	uint16_t i = 0;
	char SingleChar[5];
	uint8_t CharLength = 0;
	uint16_t XOffset = 0;
	uint16_t pIndex;
	
	while (String[i] != '\0')		//遍历字符串
	{
#ifdef LCD_CHARSET_UTF8					//定义字符集为UTF8
		/*此段代码的目的是，提取UTF8字符串中的一个字符，转存到SingleChar子字符串中*/
		/*判断UTF8编码第一个字节的标志位*/
		if ((String[i] & 0x80) == 0x00)			//第一个字节为0xxxxxxx
		{
			CharLength = 1;						//字符为1字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			SingleChar[1] = '\0';				//为SingleChar添加字符串结束标志位
		}
		else if ((String[i] & 0xE0) == 0xC0)	//第一个字节为110xxxxx
		{
			CharLength = 2;						//字符为2字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			if (String[i] == '\0') {break;}		//意外情况，跳出循环，结束显示
			SingleChar[1] = String[i ++];		//将第二个字节写入SingleChar第1个位置，随后i指向下一个字节
			SingleChar[2] = '\0';				//为SingleChar添加字符串结束标志位
		}
		else if ((String[i] & 0xF0) == 0xE0)	//第一个字节为1110xxxx
		{
			CharLength = 3;						//字符为3字节
			SingleChar[0] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[1] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[2] = String[i ++];
			SingleChar[3] = '\0';
		}
		else if ((String[i] & 0xF8) == 0xF0)	//第一个字节为11110xxx
		{
			CharLength = 4;						//字符为4字节
			SingleChar[0] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[1] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[2] = String[i ++];
			if (String[i] == '\0') {break;}
			SingleChar[3] = String[i ++];
			SingleChar[4] = '\0';
		}
		else
		{
			i ++;		//意外情况，i指向下一个字节，忽略此字节，继续判断下一个字节
			continue;
		}
#endif
		
#ifdef LCD_CHARSET_GB2312					//定义字符集为GB2312
		/*此段代码的目的是，提取GB2312字符串中的一个字符，转存到SingleChar子字符串中*/
		/*判断GB2312字节的最高位标志位*/
		if ((String[i] & 0x80) == 0x00)			//最高位为0
		{
			CharLength = 1;						//字符为1字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			SingleChar[1] = '\0';				//为SingleChar添加字符串结束标志位
		}
		else									//最高位为1
		{
			CharLength = 2;						//字符为2字节
			SingleChar[0] = String[i ++];		//将第一个字节写入SingleChar第0个位置，随后i指向下一个字节
			if (String[i] == '\0') {break;}		//意外情况，跳出循环，结束显示
			SingleChar[1] = String[i ++];		//将第二个字节写入SingleChar第1个位置，随后i指向下一个字节
			SingleChar[2] = '\0';				//为SingleChar添加字符串结束标志位
		}
#endif
		
		/*显示上述代码提取到的SingleChar*/
		if (CharLength == 1)		//如果是单字节字符
		{
			/*使用LCD_ShowChar显示此字符*/
			LCD_ShowChar(X + XOffset, Y, SingleChar[0], FontSize, Color, BackColor);
			XOffset += FontSize;
		}
		else						//否则，即多字节字符
		{
			/*遍历整个字模库，从字模库中寻找此字符的数据*/
			/*如果找到最后一个字符（定义为空字符串），则表示字符未在字模库定义，停止寻找*/
			for (pIndex = 0; strcmp(LCD_CF16x16[pIndex].Index, "") != 0; pIndex ++)
			{
				/*找到匹配的字符*/
				if (strcmp(LCD_CF16x16[pIndex].Index, SingleChar) == 0)
				{
					break;		//跳出循环，此时pIndex的值为指定字符的索引
				}
			}
			if (FontSize == LCD_8X16)	//给定字体为8*16点阵
			{
				/*将字模库LCD_CF16x16的指定数据以16*16的图像格式显示*/
				LCD_ShowImage1Bit(X + XOffset, Y, 16, 16, LCD_CF16x16[pIndex].Data, Color, BackColor);
				XOffset += 16;
			}
			else if (FontSize == LCD_6X8)	//给定字体为6*8点阵
			{
				/*空间不足，此位置显示'?'*/
				LCD_ShowChar(X + XOffset, Y, '?', LCD_6X8, Color, BackColor);
				XOffset += LCD_6X8;
			}
		}
	}
}

/**
  * 函    数：LCD显示无符号十进制数字
  * 参    数：X 指定数字左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定数字左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Number 指定要显示的数字，范围：0~4294967295
  * 参    数：Length 指定数字的长度，范围：0~10
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定数字的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定数字背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	uint8_t i;
	for (i = 0; i < Length; i ++)		//遍历数字的每一位
	{
		/*调用LCD_ShowChar显示此位数字的字符*/
		LCD_ShowChar(X + i * FontSize, Y, Number / LCD_Pow(10, Length - i - 1) % 10 + '0',
			FontSize, Color, BackColor);
	}
}

/**
  * 函    数：LCD显示有符号十进制数字
  * 参    数：X 指定数字左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定数字左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Number 指定要显示的数字，范围：-2147483648~2147483647
  * 参    数：Length 指定数字的长度，范围：0~10
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定数字的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定数字背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowSignedNum(int16_t X, int16_t Y, int32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	uint8_t i;
	uint32_t Number1;
	
	if (Number >= 0)		//数字大于等于0
	{
		LCD_ShowChar(X, Y, '+', FontSize, Color, BackColor);	//显示'+'
		Number1 = Number;										//将Number赋值给Number1
	}
	else					//数字小于0
	{
		LCD_ShowChar(X, Y, '-', FontSize, Color, BackColor);	//显示'-'
		Number1 = -Number;										//将Number取反后赋值给Number1
	}
	
	for (i = 0; i < Length; i ++)		//遍历数字的每一位
	{
		/*调用LCD_ShowChar显示此位数字的字符，注意X要后移1位，为符号预留位置*/
		LCD_ShowChar(X + (i + 1) * FontSize, Y, Number1 / LCD_Pow(10, Length - i - 1) % 10 + '0',
			FontSize, Color, BackColor);
	}
}

/**
  * 函    数：LCD显示十六进制数字
  * 参    数：X 指定数字左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定数字左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Number 指定要显示的数字，范围：0x00000000~0xFFFFFFFF
  * 参    数：Length 指定数字的长度，范围：0~8
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定数字的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定数字背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowHexNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	uint8_t i, SingleNumber;
	for (i = 0; i < Length; i ++)		//遍历数字的每一位
	{
		SingleNumber = Number / LCD_Pow(16, Length - i - 1) % 16;	//求出数字每一位的值
		
		if (SingleNumber < 10)			//数字小于10
		{
			/*数字0~9转换为字符'0'~'9'，并调用LCD_ShowChar显示*/
			LCD_ShowChar(X + i * FontSize, Y, SingleNumber + '0', FontSize, Color, BackColor);
		}
		else							//数字大于等于10
		{
			/*数字10~15转换为字符'A'~'F'，并调用LCD_ShowChar显示*/
			LCD_ShowChar(X + i * FontSize, Y, SingleNumber - 10 + 'A', FontSize, Color, BackColor);
		}
	}
}

/**
  * 函    数：LCD显示二进制数字
  * 参    数：X 指定数字左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定数字左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Number 指定要显示的数字，范围：0x00000000~0xFFFFFFFF
  * 参    数：Length 指定数字的长度，范围：0~32
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定数字的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定数字背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowBinNum(int16_t X, int16_t Y, uint32_t Number, uint8_t Length, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	uint8_t i;
	for (i = 0; i < Length; i ++)		//遍历数字的每一位
	{
		/*调用LCD_ShowChar显示此位数字的字符，数字为0显示'0'，为1显示'1'*/
		LCD_ShowChar(X + i * FontSize, Y, Number / LCD_Pow(2, Length - i - 1) % 2 + '0',
			FontSize, Color, BackColor);
	}
}

/**
  * 函    数：LCD显示浮点十进制数字
  * 参    数：X 指定数字左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定数字左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Number 指定要显示的数字，范围：-2147483648.0~2147483647.0
  * 参    数：IntLength 指定数字的整数位长度，范围：0~10
  * 参    数：FraLength 指定数字的小数位长度，范围：0~9，显示时四舍五入
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定数字的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定数字背景的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowFloatNum(int16_t X, int16_t Y, double Number, uint8_t IntLength, uint8_t FraLength, uint8_t FontSize, uint16_t Color, uint16_t BackColor)
{
	uint32_t Number1;
	
	if (Number >= 0)		//数字大于等于0
	{
		LCD_ShowChar(X, Y, '+', FontSize, Color, BackColor);	//显示'+'
	}
	else					//数字小于0
	{
		LCD_ShowChar(X, Y, '-', FontSize, Color, BackColor);	//显示'-'
		Number = -Number;										//将Number取反
	}
	X += FontSize;			//X后移1位
	
	Number1 = (uint32_t)Number;										//将整数部分赋值给Number1
	LCD_ShowNum(X, Y, Number1, IntLength, FontSize, Color, BackColor);	//显示整数部分
	X += IntLength * FontSize;										//X后移IntLength位
	
	LCD_ShowChar(X, Y, '.', FontSize, Color, BackColor);	//显示'.'
	X += FontSize;			//X后移1位
	
	Number -= Number1;											//将小数部分赋值给Number
	Number1 = (uint32_t)(Number * LCD_Pow(10, FraLength) + 0.5);	//小数部分乘10的FraLength次方，并四舍五入
	LCD_ShowNum(X, Y, Number1, FraLength, FontSize, Color, BackColor);	//显示小数部分
}

/**
  * 函    数：LCD使用printf打印字符串（支持格式化输出和中文显示）
  * 参    数：X 指定字符串左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定字符串左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：FontSize 指定字体大小
  *           范围：LCD_8X16	宽8像素，高16像素
  *                 LCD_6X8	宽6像素，高8像素
  * 参    数：Color 指定字符串的颜色，范围：0x0000~0xFFFF
  * 参    数：BackColor 指定字符串背景的颜色，范围：0x0000~0xFFFF
  * 参    数：format 指定要显示的格式化字符串，范围：ASCII码可见字符或中文字符组成的字符串
  * 参    数：... 格式化字符串参数列表
  * 返 回 值：无
  * 说    明：此函数内容和LCD_ShowString函数相同，显示的字符串不能超过64个字符
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_Printf(int16_t X, int16_t Y, uint8_t FontSize, uint16_t Color, uint16_t BackColor, const char *format, ...)
{
	char String[64];									//定义字符数组
	va_list arg;										//定义可变参数列表数据类型的变量arg
	va_start(arg, format);								//从format开始，接收参数列表到arg变量
	vsnprintf(String, sizeof(String), format, arg);		//使用vsnprintf打印格式化字符串到String
	va_end(arg);										//结束变量arg
	LCD_ShowString(X, Y, String, FontSize, Color, BackColor);	//使用LCD_ShowString显示字符串
}

/**
  * 函    数：LCD显示图像（彩色，16位每像素，RGB565格式）
  * 参    数：X 指定图像左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定图像左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定图像的宽度，范围：0~240
  * 参    数：Height 指定图像的高度，范围：0~320
  * 参    数：Image 指定要显示的图像，数据格式为逐个像素的RGB565数据
  *           数组长度需为Width*Height*2（字节数）
  * 返 回 值：无
  * 说    明：此函数用于显示彩色图像，数据格式较占用空间，显示大图时需注意空间是否够用
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowImage(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, const uint8_t *Image)
{
	uint16_t i, j;
	uint16_t Color;
	
	/*遍历指定图像的每一行*/
	for (j = 0; j < Height; j ++)
	{
		/*遍历指定图像的每一列*/
		for (i = 0; i < Width; i ++)
		{
			/*从图像数据中取出一个像素的RGB565颜色，低字节在前，高字节在后*/
			Color = Image[(j * Width + i) * 2] | ((uint16_t)Image[(j * Width + i) * 2 + 1] << 8);
			/*在指定位置画点*/
			LCD_DrawPoint(X + i, Y + j, Color);
		}
	}
}

/**
  * 函    数：LCD显示RLE压缩图像（彩色，16位每像素，RGB565格式）
  * 参    数：X 指定图像左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定图像左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定图像的宽度，范围：0~240
  * 参    数：Height 指定图像的高度，范围：0~320
  * 参    数：RLE 指定要显示的图像，数据格式为按行RLE压缩的RGB565数据
  * 返 回 值：无
  * 说    明：此函数显示效果与LCD_ShowImage函数相同，但图像数据经过RLE压缩
  *           RLE数据由若干数据包组成，每个数据包占3字节
  *           第1字节为像素个数，范围：1~255
  *           第2字节为颜色的低字节，第3字节为颜色的高字节
  *           解码时，同一行内像素个数累加达到Width后，自动进入下一行
  *           数据包不可跨越行边界，且每行的像素个数之和须等于Width
  *           颜色较单一的图像（如动画的每一帧）压缩率很高，可大幅节省Flash空间
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_ShowImageRLE(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, const uint8_t *RLE)
{
	uint16_t i, j, k;
	uint16_t Count;
	uint16_t Color;
	uint32_t Index = 0;											//RLE数据的索引
	
	/*遍历图像的每一行*/
	for (j = 0; j < Height; j ++)
	{
		i = 0;													//当前行已解码的像素个数清零
		
		/*解码一行数据，直到像素个数累加达到图像的宽度*/
		while (i < Width)
		{
			Count = RLE[Index];									//取出数据包中的像素个数
			Color = RLE[Index + 1] | ((uint16_t)RLE[Index + 2] << 8);	//取出数据包中的颜色，低字节在前
			Index += 3;											//索引后移一个数据包
			
			/*将数据包中的像素逐个写入显存数组*/
			for (k = 0; k < Count && i + k < Width; k ++)
			{
				LCD_DrawPoint(X + i + k, Y + j, Color);			//超出屏幕范围的部分不显示
			}
			i += Count;											//当前行已解码的像素个数累加
		}
	}
}

/**
  * 函    数：LCD画点
  * 参    数：X 指定点的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定点的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Color 指定点的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：此函数直接对LCD显存数组进行读写
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawPoint(int16_t X, int16_t Y, uint16_t Color)
{
	/*超出屏幕的内容不显示*/
	if (X < 0 || X >= LCD_WIDTH || Y < 0 || Y >= LCD_HEIGHT) {return;}
	
	/*将显存数组指定位置的数据改为指定颜色*/
	LCD_DisplayBuf[Y][X] = Color;
}

/**
  * 函    数：LCD读取指定位置点的颜色
  * 参    数：X 指定点的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定点的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 返 回 值：指定位置点的颜色，范围：0x0000~0xFFFF
  * 说    明：此函数直接对LCD显存数组进行读取
  */
uint16_t LCD_GetPoint(int16_t X, int16_t Y)
{
	/*超出屏幕的内容统一返回黑色*/
	if (X < 0 || X >= LCD_WIDTH || Y < 0 || Y >= LCD_HEIGHT) {return LCD_BLACK;}
	
	/*返回显存数组指定位置的数据*/
	return LCD_DisplayBuf[Y][X];
}

/**
  * 函    数：LCD画线
  * 参    数：X0 Y0 指定线的起点坐标，范围：-32768~32767，屏幕区域：0~239，0~319
  * 参    数：X1 Y1 指定线的终点坐标，范围：-32768~32767，屏幕区域：0~239，0~319
  * 参    数：Color 指定线的颜色，范围：0x0000~0xFFFF
  * 返 回 值：无
  * 说    明：此函数使用Bresenham算法画线，可以避免耗时的浮点运算，效率更高
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawLine(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, uint16_t Color)
{
	int16_t x, y, dx, dy, d, incrE, incrNE, temp;
	int16_t x0 = X0, y0 = Y0, x1 = X1, y1 = Y1;
	uint8_t yflag = 0, xyflag = 0;
	
	if (y0 == y1)		//横线单独处理
	{
		/*0号点X坐标大于1号点X坐标，则交换两点X坐标*/
		if (x0 > x1) {temp = x0; x0 = x1; x1 = temp;}
		
		/*遍历X坐标*/
		for (x = x0; x <= x1; x ++)
		{
			LCD_DrawPoint(x, y0, Color);	//依次画点
		}
	}
	else if (x0 == x1)	//竖线单独处理
	{
		/*0号点Y坐标大于1号点Y坐标，则交换两点Y坐标*/
		if (y0 > y1) {temp = y0; y0 = y1; y1 = temp;}
		
		/*遍历Y坐标*/
		for (y = y0; y <= y1; y ++)
		{
			LCD_DrawPoint(x0, y, Color);	//依次画点
		}
	}
	else				//斜线
	{
		/*使用Bresenham算法画直线，可以避免耗时的浮点运算，效率更高*/
		/*参考文档：https://www.cs.montana.edu/courses/spring2009/425/dslectures/Bresenham.pdf*/
		/*参考教程：https://www.bilibili.com/video/BV1364y1d7Lo*/
		
		if (x0 > x1)	//0号点X坐标大于1号点X坐标
		{
			/*交换两点坐标*/
			/*交换后不影响画线，但是画线方向由第一、二、三、四象限变为第一、四象限*/
			temp = x0; x0 = x1; x1 = temp;
			temp = y0; y0 = y1; y1 = temp;
		}
		
		if (y0 > y1)	//0号点Y坐标大于1号点Y坐标
		{
			/*将Y坐标取负*/
			/*取负后影响画线，但是画线方向由第一、四象限变为第一象限*/
			y0 = -y0;
			y1 = -y1;
			
			/*置标志位yflag，记住当前变换，在后续实际画线时，再将坐标换回来*/
			yflag = 1;
		}
		
		if (y1 - y0 > x1 - x0)	//画线斜率大于1
		{
			/*将X坐标与Y坐标互换*/
			/*互换后影响画线，但是画线方向由第一象限0~90度范围变为第一象限0~45度范围*/
			temp = x0; x0 = y0; y0 = temp;
			temp = x1; x1 = y1; y1 = temp;
			
			/*置标志位xyflag，记住当前变换，在后续实际画线时，再将坐标换回来*/
			xyflag = 1;
		}
		
		/*以下为Bresenham算法画直线*/
		/*算法要求，画线方向必须为第一象限0~45度范围*/
		dx = x1 - x0;
		dy = y1 - y0;
		incrE = 2 * dy;
		incrNE = 2 * (dy - dx);
		d = 2 * dy - dx;
		x = x0;
		y = y0;
		
		/*画起始点，同时判断标志位，将坐标换回来*/
		if (yflag && xyflag)	{LCD_DrawPoint(y, -x, Color);}
		else if (yflag)			{LCD_DrawPoint(x, -y, Color);}
		else if (xyflag)		{LCD_DrawPoint(y, x, Color);}
		else					{LCD_DrawPoint(x, y, Color);}
		
		while (x < x1)		//遍历X轴的每个点
		{
			x ++;
			if (d < 0)		//下一个点在当前点东方
			{
				d += incrE;
			}
			else			//下一个点在当前点东北方
			{
				y ++;
				d += incrNE;
			}
			
			/*画每一个点，同时判断标志位，将坐标换回来*/
			if (yflag && xyflag)	{LCD_DrawPoint(y, -x, Color);}
			else if (yflag)			{LCD_DrawPoint(x, -y, Color);}
			else if (xyflag)		{LCD_DrawPoint(y, x, Color);}
			else					{LCD_DrawPoint(x, y, Color);}
		}
	}
}


/**
  * 函    数：LCD画矩形
  * 参    数：X 指定矩形左上角的横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定矩形左上角的纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Width 指定矩形的宽度，范围：0~240
  * 参    数：Height 指定矩形的高度，范围：0~320
  * 参    数：Color 指定矩形的颜色，范围：0x0000~0xFFFF
  * 参    数：IsFilled 指定矩形是否填充
  *           范围：LCD_UNFILLED		不填充
  *                 LCD_FILLED		填充
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawRectangle(int16_t X, int16_t Y, uint16_t Width, uint16_t Height, uint16_t Color, uint8_t IsFilled)
{
	int16_t i, j;
	if (!IsFilled)		//指定矩形不填充
	{
		/*遍历上下X坐标，画矩形上下两条线*/
		for (i = X; i < X + Width; i ++)
		{
			LCD_DrawPoint(i, Y, Color);
			LCD_DrawPoint(i, Y + Height - 1, Color);
		}
		/*遍历左右Y坐标，画矩形左右两条线*/
		for (i = Y; i < Y + Height; i ++)
		{
			LCD_DrawPoint(X, i, Color);
			LCD_DrawPoint(X + Width - 1, i, Color);
		}
	}
	else				//指定矩形填充
	{
		/*遍历X坐标*/
		for (i = X; i < X + Width; i ++)
		{
			/*遍历Y坐标*/
			for (j = Y; j < Y + Height; j ++)
			{
				/*在指定区域画点，填充满矩形*/
				LCD_DrawPoint(i, j, Color);
			}
		}
	}
}

/**
  * 函    数：LCD画三角形
  * 参    数：X0 Y0 指定第一个端点的坐标，范围：-32768~32767，屏幕区域：0~239，0~319
  * 参    数：X1 Y1 指定第二个端点的坐标，范围：-32768~32767，屏幕区域：0~239，0~319
  * 参    数：X2 Y2 指定第三个端点的坐标，范围：-32768~32767，屏幕区域：0~239，0~319
  * 参    数：Color 指定三角形的颜色，范围：0x0000~0xFFFF
  * 参    数：IsFilled 指定三角形是否填充
  *           范围：LCD_UNFILLED		不填充
  *                 LCD_FILLED		填充
  * 返 回 值：无
  * 说    明：调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawTriangle(int16_t X0, int16_t Y0, int16_t X1, int16_t Y1, int16_t X2, int16_t Y2, uint16_t Color, uint8_t IsFilled)
{
	int16_t minx = X0, miny = Y0, maxx = X0, maxy = Y0;
	int16_t i, j;
	int16_t vx[] = {X0, X1, X2};
	int16_t vy[] = {Y0, Y1, Y2};
	
	if (!IsFilled)			//指定三角形不填充
	{
		/*调用画线函数，将三个点用直线连接*/
		LCD_DrawLine(X0, Y0, X1, Y1, Color);
		LCD_DrawLine(X0, Y0, X2, Y2, Color);
		LCD_DrawLine(X1, Y1, X2, Y2, Color);
	}
	else					//指定三角形填充
	{
		/*找到三个点最小的X、Y坐标*/
		if (X1 < minx) {minx = X1;}
		if (X2 < minx) {minx = X2;}
		if (Y1 < miny) {miny = Y1;}
		if (Y2 < miny) {miny = Y2;}
		
		/*找到三个点最大的X、Y坐标*/
		if (X1 > maxx) {maxx = X1;}
		if (X2 > maxx) {maxx = X2;}
		if (Y1 > maxy) {maxy = Y1;}
		if (Y2 > maxy) {maxy = Y2;}
		
		/*最小最大坐标之间的矩形为可能需要填充的区域*/
		/*遍历此区域中所有的点*/
		/*遍历X坐标*/
		for (i = minx; i <= maxx; i ++)
		{
			/*遍历Y坐标*/
			for (j = miny; j <= maxy; j ++)
			{
				/*调用LCD_pnpoly，判断指定点是否在指定三角形之中*/
				/*如果在，则画点，如果不在，则不做处理*/
				if (LCD_pnpoly(3, vx, vy, i, j)) {LCD_DrawPoint(i, j, Color);}
			}
		}
	}
}

/**
  * 函    数：LCD画圆
  * 参    数：X 指定圆的圆心横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定圆的圆心纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Radius 指定圆的半径，范围：0~255
  * 参    数：Color 指定圆的颜色，范围：0x0000~0xFFFF
  * 参    数：IsFilled 指定圆是否填充
  *           范围：LCD_UNFILLED		不填充
  *                 LCD_FILLED		填充
  * 返 回 值：无
  * 说    明：此函数使用Bresenham算法画圆，可以避免耗时的浮点运算，效率更高
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawCircle(int16_t X, int16_t Y, uint8_t Radius, uint16_t Color, uint8_t IsFilled)
{
	int16_t x, y, d, j;
	
	/*使用Bresenham算法画圆，可以避免耗时的浮点运算，效率更高*/
	/*参考文档：https://www.cs.montana.edu/courses/spring2009/425/dslectures/Bresenham.pdf*/
	/*参考教程：https://www.bilibili.com/video/BV1VM4y1u7wJ*/
	
	d = 1 - Radius;
	x = 0;
	y = Radius;
	
	/*画每个八分之一圆弧的起始点*/
	LCD_DrawPoint(X + x, Y + y, Color);
	LCD_DrawPoint(X - x, Y - y, Color);
	LCD_DrawPoint(X + y, Y + x, Color);
	LCD_DrawPoint(X - y, Y - x, Color);
	
	if (IsFilled)		//指定圆填充
	{
		/*遍历起始点Y坐标*/
		for (j = -y; j < y; j ++)
		{
			/*在指定区域画点，填充部分圆*/
			LCD_DrawPoint(X, Y + j, Color);
		}
	}
	
	while (x < y)		//遍历X轴的每个点
	{
		x ++;
		if (d < 0)		//下一个点在当前点东方
		{
			d += 2 * x + 1;
		}
		else			//下一个点在当前点东南方
		{
			y --;
			d += 2 * (x - y) + 1;
		}
		
		/*画每个八分之一圆弧的点*/
		LCD_DrawPoint(X + x, Y + y, Color);
		LCD_DrawPoint(X + y, Y + x, Color);
		LCD_DrawPoint(X - x, Y - y, Color);
		LCD_DrawPoint(X - y, Y - x, Color);
		LCD_DrawPoint(X + x, Y - y, Color);
		LCD_DrawPoint(X + y, Y - x, Color);
		LCD_DrawPoint(X - x, Y + y, Color);
		LCD_DrawPoint(X - y, Y + x, Color);
		
		if (IsFilled)	//指定圆填充
		{
			/*遍历中间部分*/
			for (j = -y; j < y; j ++)
			{
				/*在指定区域画点，填充部分圆*/
				LCD_DrawPoint(X + x, Y + j, Color);
				LCD_DrawPoint(X - x, Y + j, Color);
			}
			
			/*遍历两侧部分*/
			for (j = -x; j < x; j ++)
			{
				/*在指定区域画点，填充部分圆*/
				LCD_DrawPoint(X - y, Y + j, Color);
				LCD_DrawPoint(X + y, Y + j, Color);
			}
		}
	}
}

/**
  * 函    数：LCD画椭圆
  * 参    数：X 指定椭圆的圆心横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定椭圆的圆心纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：A 指定椭圆的横向半轴长度，范围：0~255
  * 参    数：B 指定椭圆的纵向半轴长度，范围：0~255
  * 参    数：Color 指定椭圆的颜色，范围：0x0000~0xFFFF
  * 参    数：IsFilled 指定椭圆是否填充
  *           范围：LCD_UNFILLED		不填充
  *                 LCD_FILLED		填充
  * 返 回 值：无
  * 说    明：此函数使用Bresenham算法画椭圆，可以避免部分耗时的浮点运算，效率更高
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawEllipse(int16_t X, int16_t Y, uint8_t A, uint8_t B, uint16_t Color, uint8_t IsFilled)
{
	int16_t x, y, j;
	int16_t a = A, b = B;
	float d1, d2;
	
	/*使用Bresenham算法画椭圆，可以避免部分耗时的浮点运算，效率更高*/
	/*参考链接：https://blog.csdn.net/myf_666/article/details/128167392*/
	
	x = 0;
	y = b;
	d1 = b * b + a * a * (-b + 0.5);
	
	if (IsFilled)	//指定椭圆填充
	{
		/*遍历起始点Y坐标*/
		for (j = -y; j < y; j ++)
		{
			/*在指定区域画点，填充部分椭圆*/
			LCD_DrawPoint(X, Y + j, Color);
		}
	}
	
	/*画椭圆弧的起始点*/
	LCD_DrawPoint(X + x, Y + y, Color);
	LCD_DrawPoint(X - x, Y - y, Color);
	LCD_DrawPoint(X - x, Y + y, Color);
	LCD_DrawPoint(X + x, Y - y, Color);
	
	/*画椭圆中间部分*/
	while (b * b * (x + 1) < a * a * (y - 0.5))
	{
		if (d1 <= 0)		//下一个点在当前点东方
		{
			d1 += b * b * (2 * x + 3);
		}
		else				//下一个点在当前点东南方
		{
			d1 += b * b * (2 * x + 3) + a * a * (-2 * y + 2);
			y --;
		}
		x ++;
		
		if (IsFilled)	//指定椭圆填充
		{
			/*遍历中间部分*/
			for (j = -y; j < y; j ++)
			{
				/*在指定区域画点，填充部分椭圆*/
				LCD_DrawPoint(X + x, Y + j, Color);
				LCD_DrawPoint(X - x, Y + j, Color);
			}
		}
		
		/*画椭圆中间部分圆弧*/
		LCD_DrawPoint(X + x, Y + y, Color);
		LCD_DrawPoint(X - x, Y - y, Color);
		LCD_DrawPoint(X - x, Y + y, Color);
		LCD_DrawPoint(X + x, Y - y, Color);
	}
	
	/*画椭圆两侧部分*/
	d2 = b * b * (x + 0.5) * (x + 0.5) + a * a * (y - 1) * (y - 1) - a * a * b * b;
	
	while (y > 0)
	{
		if (d2 <= 0)		//下一个点在当前点东方
		{
			d2 += b * b * (2 * x + 2) + a * a * (-2 * y + 3);
			x ++;
		}
		else				//下一个点在当前点东南方
		{
			d2 += a * a * (-2 * y + 3);
		}
		y --;
		
		if (IsFilled)	//指定椭圆填充
		{
			/*遍历两侧部分*/
			for (j = -y; j < y; j ++)
			{
				/*在指定区域画点，填充部分椭圆*/
				LCD_DrawPoint(X + x, Y + j, Color);
				LCD_DrawPoint(X - x, Y + j, Color);
			}
		}
		
		/*画椭圆两侧部分圆弧*/
		LCD_DrawPoint(X + x, Y + y, Color);
		LCD_DrawPoint(X - x, Y - y, Color);
		LCD_DrawPoint(X - x, Y + y, Color);
		LCD_DrawPoint(X + x, Y - y, Color);
	}
}

/**
  * 函    数：LCD画圆弧
  * 参    数：X 指定圆弧的圆心横坐标，范围：-32768~32767，屏幕区域：0~239
  * 参    数：Y 指定圆弧的圆心纵坐标，范围：-32768~32767，屏幕区域：0~319
  * 参    数：Radius 指定圆弧的半径，范围：0~255
  * 参    数：StartAngle 指定圆弧的起始角度，范围：-180~180
  *           水平向右为0度，水平向左为180度或-180度，下方为正数，上方为负数，顺时针旋转
  * 参    数：EndAngle 指定圆弧的终止角度，范围：-180~180
  *           水平向右为0度，水平向左为180度或-180度，下方为正数，上方为负数，顺时针旋转
  * 参    数：Color 指定圆弧的颜色，范围：0x0000~0xFFFF
  * 参    数：IsFilled 指定圆弧是否填充，填充后为扇形
  *           范围：LCD_UNFILLED		不填充
  *                 LCD_FILLED		填充
  * 返 回 值：无
  * 说    明：此函数借用Bresenham算法画圆的方法
  *           调用此函数后，要想真正地呈现在屏幕上，还需调用更新函数
  */
void LCD_DrawArc(int16_t X, int16_t Y, uint8_t Radius, int16_t StartAngle, int16_t EndAngle, uint16_t Color, uint8_t IsFilled)
{
	int16_t x, y, d, j;
	
	/*此函数借用Bresenham算法画圆的方法*/
	
	d = 1 - Radius;
	x = 0;
	y = Radius;
	
	/*在画圆的每个点时，判断指定点是否在指定角度内，在，则画点，不在，则不做处理*/
	if (LCD_IsInAngle(x, y, StartAngle, EndAngle)) {LCD_DrawPoint(X + x, Y + y, Color);}
	if (LCD_IsInAngle(-x, -y, StartAngle, EndAngle)) {LCD_DrawPoint(X - x, Y - y, Color);}
	if (LCD_IsInAngle(y, x, StartAngle, EndAngle)) {LCD_DrawPoint(X + y, Y + x, Color);}
	if (LCD_IsInAngle(-y, -x, StartAngle, EndAngle)) {LCD_DrawPoint(X - y, Y - x, Color);}
	
	if (IsFilled)	//指定圆弧填充
	{
		/*遍历起始点Y坐标*/
		for (j = -y; j < y; j ++)
		{
			/*在填充圆的每个点时，判断指定点是否在指定角度内，在，则画点，不在，则不做处理*/
			if (LCD_IsInAngle(0, j, StartAngle, EndAngle)) {LCD_DrawPoint(X, Y + j, Color);}
		}
	}
	
	while (x < y)		//遍历X轴的每个点
	{
		x ++;
		if (d < 0)		//下一个点在当前点东方
		{
			d += 2 * x + 1;
		}
		else			//下一个点在当前点东南方
		{
			y --;
			d += 2 * (x - y) + 1;
		}
		
		/*在画圆的每个点时，判断指定点是否在指定角度内，在，则画点，不在，则不做处理*/
		if (LCD_IsInAngle(x, y, StartAngle, EndAngle)) {LCD_DrawPoint(X + x, Y + y, Color);}
		if (LCD_IsInAngle(y, x, StartAngle, EndAngle)) {LCD_DrawPoint(X + y, Y + x, Color);}
		if (LCD_IsInAngle(-x, -y, StartAngle, EndAngle)) {LCD_DrawPoint(X - x, Y - y, Color);}
		if (LCD_IsInAngle(-y, -x, StartAngle, EndAngle)) {LCD_DrawPoint(X - y, Y - x, Color);}
		if (LCD_IsInAngle(x, -y, StartAngle, EndAngle)) {LCD_DrawPoint(X + x, Y - y, Color);}
		if (LCD_IsInAngle(y, -x, StartAngle, EndAngle)) {LCD_DrawPoint(X + y, Y - x, Color);}
		if (LCD_IsInAngle(-x, y, StartAngle, EndAngle)) {LCD_DrawPoint(X - x, Y + y, Color);}
		if (LCD_IsInAngle(-y, x, StartAngle, EndAngle)) {LCD_DrawPoint(X - y, Y + x, Color);}
		
		if (IsFilled)	//指定圆弧填充
		{
			/*遍历中间部分*/
			for (j = -y; j < y; j ++)
			{
				/*在填充圆的每个点时，判断指定点是否在指定角度内，在，则画点，不在，则不做处理*/
				if (LCD_IsInAngle(x, j, StartAngle, EndAngle)) {LCD_DrawPoint(X + x, Y + j, Color);}
				if (LCD_IsInAngle(-x, j, StartAngle, EndAngle)) {LCD_DrawPoint(X - x, Y + j, Color);}
			}
			
			/*遍历两侧部分*/
			for (j = -x; j < x; j ++)
			{
				/*在填充圆的每个点时，判断指定点是否在指定角度内，在，则画点，不在，则不做处理*/
				if (LCD_IsInAngle(-y, j, StartAngle, EndAngle)) {LCD_DrawPoint(X - y, Y + j, Color);}
				if (LCD_IsInAngle(y, j, StartAngle, EndAngle)) {LCD_DrawPoint(X + y, Y + j, Color);}
			}
		}
	}
}

/*********************功能函数*/
