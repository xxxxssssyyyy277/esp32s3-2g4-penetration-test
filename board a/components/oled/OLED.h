#ifndef __OLED_H
#define __OLED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "OLED_Data.h"

/* ================= 引脚与总线参数 =================
 * 板 A 固定接线：SCL = GPIO1（模块丝印 SCK，同一根时钟线）、SDA = GPIO2。
 * 组件不能包含 main/app_config.h，所以这三个宏定义在这里，
 * 需要改引脚时用 idf.py -DOLED_SCL_PIN=<n> 覆盖。
 * 不要用 GPIO37（Octal PSRAM 的 DQS/DM）、GPIO38（FSPIWP）、GPIO9-14（SPI2 IOMUX）。 */
#ifndef OLED_SCL_PIN
#define OLED_SCL_PIN        1
#endif
#ifndef OLED_SDA_PIN
#define OLED_SDA_PIN        2
#endif
/* 每次电平变化后的延时（us）。ESP32-S3 @240 MHz 下必须非 0，
 * 否则位翻转速率超过 SSD1306 的 400 kHz 上限 -> 不亮或花屏。 */
#ifndef OLED_I2C_DELAY_US
#define OLED_I2C_DELAY_US   1
#endif

/* SSD1306 的 7 位地址 0x3C 左移一位得到写地址 0x78；有些模块焊的是 0x3D -> 0x7A。
 * OLED_Init() 会先探测这两个地址，用有 ACK 的那个。 */
#define OLED_I2C_ADDR       0x78
#define OLED_I2C_ADDR_ALT   0x7A

/* FontSize 参数取值 */
#define OLED_8X16           8
#define OLED_6X8            6

/* IsFilled 参数取值 */
#define OLED_UNFILLED       0
#define OLED_FILLED         1

/* 显存：8 页 × 128 列，1 bit = 1 像素 */
extern uint8_t OLED_DisplayBuf[8][128];

/* 初始化并探测从机地址：返回有 ACK 的 8 位写地址（0x78 / 0x7A），无应答返回 0。
 *
 * 注意：返回 0 不代表「什么都没发」——初始化序列到这里已经照发过了（参考实现
 * 江协 V2.0 根本不读 ACK 也能点亮屏）。调用方可以继续渲染，写总线是无害的。
 * 本函数可重复调用（屏热插拔/后上电时用来补初始化）。 */
uint8_t OLED_Init(void);

/* 当前使用的位延时（us），探测失败/降速时可用它打日志 */
uint32_t OLED_GetDelayUs(void);

/* 最近一次 OLED_Init() 期间收到 ACK 的字节数（含盲发初始化阶段）。
 * > 0 说明屏确实在总线上应答了。 */
uint32_t OLED_GetAckCount(void);

/* 最近一次 OLED_Init() 期间没收到 ACK 的字节数 */
uint32_t OLED_GetNackCount(void);

/* 当前实际使用的引脚（初始化时可能因 SCL/SDA 自动互换而变化） */
uint8_t OLED_GetSclPin(void);
uint8_t OLED_GetSdaPin(void);

/* 轻量探测：只对当前地址发一个地址字节，返回是否 ACK。
 * 屏没上线时每 2 s 用它查一次，通了再走完整 OLED_Init()，避免反复清屏。 */
bool OLED_Probe(void);

/* 总线空闲电平，返回 (SCL << 1) | SDA。
 * 释放总线后两位都应为 1；某位是 0 说明线上被拉死（模块没供电、短路或接错脚）。 */
uint8_t OLED_GetLineLevels(void);

/* 诊断用：只开内部下拉去读 SCL/SDA，返回 (SCL << 1) | SDA。
 * 读到 1 说明线上有模块自带的 4.7k 上拉（模块确实接着且有电）；
 * 读到 0 说明这两个脚上什么也没挂。 */
uint8_t OLED_CheckPullups(void);

/* 诊断用：一组 (SCL, SDA, 地址) 命中记录，地址是 8 位写地址（0x78/0x7A） */
typedef struct {
    uint8_t scl;
    uint8_t sda;
    uint8_t addr;
} oled_scan_hit_t;

/* 本固件是否编进了诊断扫描（-DOLED_SCAN_ALL=1）。
 * 显示层用它决定要不要做周期性的“上拉指纹”检查，免得正式固件多打日志。 */
bool OLED_DiagScanEnabled(void);

/* 诊断用：一组 (位延时, 地址) 结果，addr 为 0 表示这一档没应答 */
typedef struct {
    uint32_t delay_us;
    uint8_t  addr;      /* 8 位写地址（0x78/0x7A）；0 = 这一档没应答 */
} oled_speed_result_t;

/* 诊断用：在当前引脚上把位延时从 1 us 逐档放到 2 ms，每档 ping 一次 0x3C/0x3D，
 * 把每档结果写进 out（最多 max 条）并返回条数。
 * 用途：模块没有板载上拉、线又长时，只能靠 ESP32 内部 45 kΩ 上拉（很弱），
 * 上升沿慢到高速下必然出错；这一支能判定「线接对了、只是总线太快」。
 * 需要 -DOLED_SCAN_ALL=1 才会真正测，否则恒返回 0。 */
size_t OLED_SpeedSweep(oled_speed_result_t *out, size_t max);

/* 诊断用：逐脚开「内部下拉 + 输入」读电平，返回读成高电平的引脚 ——
 * 也就是线上存在外部上拉（模块自带的 4.7 kΩ 就挂在 SCL/SDA 上）。
 * 屏有电且与 ESP32 共地时，这里会直接列出屏实际接的那两根脚；
 * 一个都没有 ⇒ 模块没供电 / 没共地 / 根本没接到板子上。
 * 需要 -DOLED_SCAN_ALL=1 重新编译才会真正测，否则恒返回 0。 */
size_t OLED_ScanPullups(uint8_t *out, size_t max);

/* 诊断用：把所有能安全当 GPIO 的引脚两两组合成 (SCL, SDA) 去探测，
 * 命中的组合写进 out，返回命中个数。
 * 需要 -DOLED_SCAN_ALL=1 重新编译才会真正扫描，否则恒返回 0。 */
size_t OLED_ScanPins(oled_scan_hit_t *out, size_t max);

/* 把整块显存或指定矩形推送到屏上 */
void OLED_Update(void);
void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/* 只改显存，不推送 */
void OLED_Clear(void);
void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height);

/* 只支持 OLED_8X16（汉字字库已删除，Screen 内容全为 ASCII） */
void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize);
void OLED_ShowString(int16_t X, int16_t Y, const char *String, uint8_t FontSize);
void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image);

#endif /* __OLED_H */
