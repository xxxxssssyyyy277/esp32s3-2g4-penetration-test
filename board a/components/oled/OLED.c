#include "OLED.h"

#include <stdbool.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_rom_sys.h"

/* 显存：8 页 × 128 列，每 bit 一个像素，bit0 在上 */
uint8_t OLED_DisplayBuf[8][128];

/* ===================== 软件 I2C 底层 ===================== */

/* 运行时写地址：OLED_Init() 探测到哪个地址就存哪个，默认 0x78 */
static uint8_t s_i2c_addr = OLED_I2C_ADDR;

/* 运行时位延时：探测阶段如果 1 us 没人应答，会逐档放慢到 5 us / 20 us。
 * 模块上拉偏弱、杜邦线偏长时，慢一点往往就能通。 */
static uint32_t s_delay_us = OLED_I2C_DELAY_US;

/* 运行时引脚号：默认取 OLED.h 里的宏，诊断扫描时会临时改成别的组合 */
static uint8_t s_scl_pin = OLED_SCL_PIN;
static uint8_t s_sda_pin = OLED_SDA_PIN;

/* 最近一次 OLED_Init() 期间，总线上收到 / 没收到 ACK 的字节数。
 * 用途：江协的原版驱动根本不读 ACK 也能点亮，所以这里不会因为"没读到 ACK"
 * 就放弃初始化，而是照发命令，再用这两个计数判断屏到底在不在线上。 */
static uint32_t s_ack_ok;
static uint32_t s_ack_fail;

/* 第 9 个时钟上采样 SDA 前的稳定时间（us）。
 * 只靠内部弱上拉（约 45 kΩ）时 SDA 上升沿较慢，2 us 在长杜邦线/面包板上
 * 可能还没爬到高电平，会把 NACK 误判成 ACK；给到 10 us 就与线长无关了。 */
#define OLED_ACK_SETTLE_US  10

static void OLED_W_SCL(uint8_t BitValue)
{
    gpio_set_level((gpio_num_t)s_scl_pin, BitValue);
    /* 关键：ESP32-S3 主频高，电平间不加延时会把 I2C 跑到 400 kHz 以上 */
    esp_rom_delay_us(s_delay_us);
}

static void OLED_W_SDA(uint8_t BitValue)
{
    gpio_set_level((gpio_num_t)s_sda_pin, BitValue);
    esp_rom_delay_us(s_delay_us);
}

/* 按当前 s_scl_pin / s_sda_pin 重新配置总线并释放，
 * 顺带把「从机把 SDA 拉死」的情况用 9 个时钟救回来。 */
static void OLED_BusBegin(void)
{
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_scl_pin) | (1ULL << s_sda_pin),
        /* 开漏输出；模块一般自带 4.7k 上拉，这里再开内部弱上拉兜底 */
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));

    /* 释放总线，等 OLED 供电稳定 */
    OLED_W_SCL(1);
    OLED_W_SDA(1);
    esp_rom_delay_us(2000);

    if (gpio_get_level((gpio_num_t)s_sda_pin) == 0) {
        for (int i = 0; i < 9; i++) {
            OLED_W_SCL(0);
            OLED_W_SCL(1);
        }
        OLED_W_SDA(0);
        OLED_W_SCL(1);
        OLED_W_SDA(1);
        esp_rom_delay_us(10);
    }
}

static void OLED_I2C_Start(void)
{
    OLED_W_SDA(1);
    OLED_W_SCL(1);
    OLED_W_SDA(0);
    OLED_W_SCL(0);
}

static void OLED_I2C_Stop(void)
{
    OLED_W_SDA(0);
    OLED_W_SCL(1);
    OLED_W_SDA(1);
}

/* 返回第 9 个时钟上从机的 ACK（SDA 被拉低为 true） */
static bool OLED_I2C_SendByte(uint8_t Byte)
{
    for (uint8_t i = 0; i < 8; i++) {
        OLED_W_SDA(!!(Byte & (0x80 >> i)));
        OLED_W_SCL(1);
        OLED_W_SCL(0);
    }
    /* 第 9 个时钟：先释放 SDA，再放开 SCL，然后在 SCL 高电平期间读 SDA。
     * 三点必须照做：
     *   1. 读运行时引脚 s_sda_pin —— 原来误用了编译期宏 OLED_SDA_PIN，
     *      导致 OLED_ScanPins() 扫引脚时永远读另一根脚，扫描结论全部作废；
     *   2. 采样前多等 OLED_ACK_SETTLE_US —— 只靠内部弱上拉（约 45 kΩ）时
     *      长线上的 SDA 爬升慢，等太短会把 NACK 误判成 ACK；
     *   3. 必须在 SCL 拉低之前采样。 */
    OLED_W_SDA(1);
    OLED_W_SCL(1);
    esp_rom_delay_us(OLED_ACK_SETTLE_US);
    bool ack = (gpio_get_level((gpio_num_t)s_sda_pin) == 0);
    OLED_W_SCL(0);
    return ack;
}

/* 只发地址字节，用来看某个地址上有没有器件 */
static bool OLED_I2C_Ping(uint8_t Addr7)
{
    OLED_I2C_Start();
    bool ack = OLED_I2C_SendByte((uint8_t)(Addr7 << 1));
    OLED_I2C_Stop();
    return ack;
}

static void OLED_WriteCommand(uint8_t Command)
{
    OLED_I2C_Start();
    bool ok = OLED_I2C_SendByte(s_i2c_addr);
    if (!OLED_I2C_SendByte(0x00)) {     /* 控制字节 0x00：后面是命令 */
        ok = false;
    }
    if (!OLED_I2C_SendByte(Command)) {
        ok = false;
    }
    OLED_I2C_Stop();

    if (ok) {
        s_ack_ok++;
    } else {
        s_ack_fail++;
    }
}

static void OLED_WriteData(const uint8_t *Data, uint8_t Count)
{
    OLED_I2C_Start();
    bool ok = OLED_I2C_SendByte(s_i2c_addr);
    if (!OLED_I2C_SendByte(0x40)) {     /* 控制字节 0x40：后面是显存数据 */
        ok = false;
    }
    for (uint8_t i = 0; i < Count; i++) {
        if (!OLED_I2C_SendByte(Data[i])) {
            ok = false;
        }
    }
    OLED_I2C_Stop();

    if (ok) {
        s_ack_ok++;
    } else {
        s_ack_fail++;
    }
}

/* 光标定位：page 0~7，列 0~127 */
static void OLED_SetCursor(uint8_t Page, uint8_t X)
{
    OLED_WriteCommand(0xB0 | Page);
    OLED_WriteCommand(0x10 | ((X & 0xF0) >> 4));
    OLED_WriteCommand(0x00 | (X & 0x0F));
}

/* 探测从机：0x3C / 0x3D 谁应答就用谁。刚上电的模块可能还没准备好，
 * 所以先按 1 us 试两次，再放慢到 5 us、20 us 各试一次（上拉弱、线长时有效）。 */
static uint8_t OLED_ProbeAddr(void)
{
    static const uint32_t k_delay_steps[4] = { OLED_I2C_DELAY_US, OLED_I2C_DELAY_US, 5, 20 };
    for (int attempt = 0; attempt < 4; attempt++) {
        s_delay_us = k_delay_steps[attempt];
        if (OLED_I2C_Ping(0x3C)) {
            return (uint8_t)(0x3C << 1);
        }
        if (OLED_I2C_Ping(0x3D)) {
            return (uint8_t)(0x3D << 1);
        }
        esp_rom_delay_us(20000);
    }
    return 0;
}

uint8_t OLED_Init(void)
{
    s_ack_ok = 0;
    s_ack_fail = 0;

    /* 1) 探测：先按默认映射（SCL = OLED_SCL_PIN，SDA = OLED_SDA_PIN）试，
     *    没人应答再把 SCL/SDA 交换一次 —— 杜邦线插反是"完全没反应"的常见原因，
     *    换过来还能通的话就不用改接线。 */
    static const uint8_t k_scl_try[2] = { OLED_SCL_PIN, OLED_SDA_PIN };
    static const uint8_t k_sda_try[2] = { OLED_SDA_PIN, OLED_SCL_PIN };

    uint8_t found = 0;
    for (int map = 0; map < 2 && found == 0; map++) {
        s_scl_pin = k_scl_try[map];
        s_sda_pin = k_sda_try[map];
        s_delay_us = OLED_I2C_DELAY_US;
        OLED_BusBegin();
        esp_rom_delay_us(50000);        /* 等模块电源稳定再开口（参考实现同样先等） */
        found = OLED_ProbeAddr();
    }

    if (found != 0) {
        s_i2c_addr = found;             /* s_delay_us 停在能通的那一档 */
    } else {
        /* 2) 没有 ACK 也不放弃。参考实现（江协 V2.0）压根不读 ACK，照样能点亮屏；
         *    我们的驱动不该因为"没读到 ACK"就把整段初始化跳过——那样屏永远不会亮。
         *    这里回到默认映射，照发初始化序列：屏真的在线上就会亮。 */
        s_scl_pin = OLED_SCL_PIN;
        s_sda_pin = OLED_SDA_PIN;
        s_delay_us = OLED_I2C_DELAY_US;
        s_i2c_addr = OLED_I2C_ADDR;
        OLED_BusBegin();
        esp_rom_delay_us(10000);
    }

    /* 3) 初始化命令序列：与参考实现 Hardware/OLED.c 的 OLED_Init() 逐条一致。
     *    参考实现能点亮同一块屏，这里就不增删任何一条。
     *    （别加 0xAD/0x8B：那是 SH1106 的 DC-DC，SSD1306 上 0xAD 是内部 IREF 设置，
     *      语义不同，实测可用的参考序列里没有它。） */
    OLED_WriteCommand(0xAE);        /* 关显示 */

    OLED_WriteCommand(0xD5);        /* 时钟分频/振荡频率 */
    OLED_WriteCommand(0x80);

    OLED_WriteCommand(0xA8);        /* 多路复用率 */
    OLED_WriteCommand(0x3F);

    OLED_WriteCommand(0xD3);        /* 显示偏移 */
    OLED_WriteCommand(0x00);

    OLED_WriteCommand(0x40);        /* 显示起始行 */

    OLED_WriteCommand(0xA1);        /* 左右方向：正常 */
    OLED_WriteCommand(0xC8);        /* 上下方向：正常 */

    OLED_WriteCommand(0xDA);        /* COM 引脚硬件配置 */
    OLED_WriteCommand(0x12);

    OLED_WriteCommand(0x81);        /* 对比度 */
    OLED_WriteCommand(0xCF);

    OLED_WriteCommand(0xD9);        /* 预充电周期 */
    OLED_WriteCommand(0xF1);

    OLED_WriteCommand(0xDB);        /* VCOMH */
    OLED_WriteCommand(0x30);

    OLED_WriteCommand(0xA4);        /* 显示跟随显存 */
    OLED_WriteCommand(0xA6);        /* 正常显示（非反色） */

    OLED_WriteCommand(0x8D);        /* 电荷泵（SSD1306 必需） */
    OLED_WriteCommand(0x14);

    OLED_WriteCommand(0xAF);        /* 开显示 */

    /* 4) 清屏 + 整屏刷新，防止初始化后花屏 */
    OLED_Clear();
    OLED_Update();

    /* 5) 探测没抓到 ACK，但盲发初始化时收到了：说明屏其实在线上
     *    （只是探测那一下没响应），同样算初始化成功。 */
    if (found == 0 && s_ack_ok > 0) {
        found = s_i2c_addr;
    }

    return found;
}

uint32_t OLED_GetDelayUs(void)
{
    return s_delay_us;
}

bool OLED_Probe(void)
{
    /* 轻量探测：只发一个地址字节，不发初始化序列。
     * 用法：屏当初没应答时，显示任务每 2 s 用它查一次，
     * 查到 ACK 再走完整的 OLED_Init()，避免反复清屏导致闪屏。
     * 两个地址都试：屏可能是后插上的 0x3D 模块，只试 0x3C 会永远探不到。 */
    if (OLED_I2C_Ping((uint8_t)(s_i2c_addr >> 1))) {
        return true;
    }

    uint8_t alt7 = (s_i2c_addr == OLED_I2C_ADDR_ALT) ? 0x3C : 0x3D;
    if (OLED_I2C_Ping(alt7)) {
        s_i2c_addr = (uint8_t)(alt7 << 1);     /* 换到有应答的那个地址 */
        return true;
    }
    return false;
}

uint32_t OLED_GetAckCount(void)
{
    return s_ack_ok;
}

uint32_t OLED_GetNackCount(void)
{
    return s_ack_fail;
}

uint8_t OLED_GetSclPin(void)
{
    return s_scl_pin;
}

uint8_t OLED_GetSdaPin(void)
{
    return s_sda_pin;
}

uint8_t OLED_GetLineLevels(void)
{
    /* 先确保两条线都处于「释放」状态（开漏输出 1 = 高阻，由上拉拉高） */
    OLED_W_SCL(1);
    OLED_W_SDA(1);
    esp_rom_delay_us(5);

    uint8_t scl = (uint8_t)(gpio_get_level((gpio_num_t)s_scl_pin) ? 1 : 0);
    uint8_t sda = (uint8_t)(gpio_get_level((gpio_num_t)s_sda_pin) ? 1 : 0);
    return (uint8_t)((scl << 1) | sda);
}

uint8_t OLED_CheckPullups(void)
{
    /* 只开内部下拉、关掉上拉：如果线上接着模块自带的 4.7k 上拉，
     * 分压后仍是高电平（45k : 4.7k -> 约 3.0 V）；
     * 如果这两个脚上什么都没接，只有芯片内部 45k 下拉 -> 读 0。
     * 用它区分「模块没挂上/没供电」和「模块在但不应答」。 */
    gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << s_scl_pin) | (1ULL << s_sda_pin),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cfg));
    esp_rom_delay_us(200);

    uint8_t scl = (uint8_t)(gpio_get_level((gpio_num_t)s_scl_pin) ? 1 : 0);
    uint8_t sda = (uint8_t)(gpio_get_level((gpio_num_t)s_sda_pin) ? 1 : 0);

    OLED_BusBegin();    /* 恢复成开漏输出 + 上拉 */
    return (uint8_t)((scl << 1) | sda);
}

#ifdef OLED_SCAN_ALL
/* 可安全当普通 GPIO 用的引脚（跳过 19/20 USB、26–32 Flash、33–37 Octal PSRAM、
 * 38 FSPIWP、39–42 JTAG、43/44 UART0、46 只输入）。
 * 也跳过 GPIO0：它是 BOOT 按键的 strapping 脚，板上通常并了一颗去抖电容，
 * 拉低后 10 µs 都回不到高电平 —— 会被误判成“任何地址都有 ACK”。
 * 共 21 颗，两两组合正好 21×20 = 420 种。 */
static const uint8_t k_scan_pins[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
    13, 14, 15, 16, 17, 18, 21, 47, 48
};
#endif

size_t OLED_ScanPins(oled_scan_hit_t *out, size_t max)
{
#ifdef OLED_SCAN_ALL
    if (out == NULL || max == 0) {
        return 0;
    }

    size_t n = 0;
    const size_t cnt = sizeof(k_scan_pins) / sizeof(k_scan_pins[0]);
    for (size_t i = 0; i < cnt && n < max; i++) {
        for (size_t j = 0; j < cnt && n < max; j++) {
            if (i == j) {
                continue;   /* SCL 和 SDA 不能是同一个脚 */
            }
            s_scl_pin = k_scan_pins[i];
            s_sda_pin = k_scan_pins[j];
            s_delay_us = 5;
            OLED_BusBegin();

            /* SDA 空闲不是高电平就先跳过：要么上一条组合把线拉死了，要么这根脚
             * 短地。这种情况下地址字节后面的“ACK”必然是假的（SDA 一直是低）。 */
            if (gpio_get_level((gpio_num_t)s_sda_pin) == 0) {
                continue;
            }

            uint8_t addr = 0;
            if (OLED_I2C_Ping(0x3C)) {
                addr = 0x78;
            } else if (OLED_I2C_Ping(0x3D)) {
                addr = 0x7A;
            }
            if (addr != 0) {
                out[n].scl  = s_scl_pin;
                out[n].sda  = s_sda_pin;
                out[n].addr = addr;
                n++;
            }
        }
    }

    /* 恢复默认引脚映射，屏仍然按 OLED.h 的宏工作 */
    s_scl_pin = OLED_SCL_PIN;
    s_sda_pin = OLED_SDA_PIN;
    s_delay_us = OLED_I2C_DELAY_US;
    OLED_BusBegin();
    return n;
#else
    (void)out;
    (void)max;
    return 0;
#endif
}

bool OLED_DiagScanEnabled(void)
{
#ifdef OLED_SCAN_ALL
    return true;
#else
    return false;
#endif
}

size_t OLED_SpeedSweep(oled_speed_result_t *out, size_t max)
{
#ifdef OLED_SCAN_ALL
    /* 1 us 约等于 330 kHz（一个 bit 三次 1 us 延时），2000 us 约等于 250 Hz：
     * 慢到连 45 kΩ 上拉都能把线拉起来 */
    static const uint32_t k_delays[] = { 1, 2, 5, 10, 20, 50, 100, 200, 500, 1000, 2000 };

    if (out == NULL || max == 0) {
        return 0;
    }

    size_t n = 0;
    for (size_t i = 0; i < sizeof(k_delays) / sizeof(k_delays[0]) && n < max; i++) {
        s_delay_us = k_delays[i];
        OLED_BusBegin();

        uint8_t addr = 0;
        if (OLED_I2C_Ping(0x3C)) {
            addr = (uint8_t)(0x3C << 1);
        } else if (OLED_I2C_Ping(0x3D)) {
            addr = (uint8_t)(0x3D << 1);
        }
        out[n].delay_us = k_delays[i];
        out[n].addr     = addr;
        n++;
    }

    /* 扫描完回到默认状态，别把慢速留在总线上 */
    s_scl_pin  = OLED_SCL_PIN;
    s_sda_pin  = OLED_SDA_PIN;
    s_delay_us = OLED_I2C_DELAY_US;
    OLED_BusBegin();
    return n;
#else
    (void)out;
    (void)max;
    return 0;
#endif
}

size_t OLED_ScanPullups(uint8_t *out, size_t max)
{
#ifdef OLED_SCAN_ALL
    if (out == NULL || max == 0) {
        return 0;
    }

    size_t n = 0;
    const size_t cnt = sizeof(k_scan_pins) / sizeof(k_scan_pins[0]);
    for (size_t i = 0; i < cnt; i++) {
        uint8_t pin = k_scan_pins[i];

        /* 只开内部下拉（约 45 kΩ）：什么都没接的脚会被拉到 0；
         * 挂着模块 4.7 kΩ 上拉的脚会被拉回高电平（分压后约 3.0 V）。 */
        gpio_config_t cfg = {
            .pin_bit_mask = (1ULL << pin),
            .mode         = GPIO_MODE_INPUT,
            .pull_up_en   = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,
            .intr_type    = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&cfg));
        esp_rom_delay_us(500);

        if (gpio_get_level((gpio_num_t)pin) != 0 && n < max) {
            out[n++] = pin;
        }
        gpio_reset_pin((gpio_num_t)pin);    /* 放回默认态，别把内部下拉留在这根脚上 */
    }

    /* 恢复总线引脚与开漏输出，后面的渲染照常进行 */
    s_scl_pin = OLED_SCL_PIN;
    s_sda_pin = OLED_SDA_PIN;
    s_delay_us = OLED_I2C_DELAY_US;
    OLED_BusBegin();
    return n;
#else
    (void)out;
    (void)max;
    return 0;
#endif
}

/* ===================== 显存 -> 屏 ===================== */

void OLED_Update(void)
{
    for (uint8_t page = 0; page < 8; page++) {
        OLED_SetCursor(page, 0);
        OLED_WriteData(OLED_DisplayBuf[page], 128);
    }
}

void OLED_UpdateArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)
{
    if (Width == 0 || Height == 0) {
        return;
    }

    int16_t x0 = (X < 0) ? 0 : X;
    int16_t x1 = (int16_t)(X + Width);
    if (x1 > 128) {
        x1 = 128;
    }
    int16_t y0 = (Y < 0) ? 0 : Y;
    int16_t y1 = (int16_t)(Y + Height);
    if (y1 > 64) {
        y1 = 64;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    uint8_t page0 = (uint8_t)(y0 / 8);
    uint8_t page1 = (uint8_t)((y1 - 1) / 8);

    for (uint8_t page = page0; page <= page1; page++) {
        OLED_SetCursor(page, (uint8_t)x0);
        OLED_WriteData(&OLED_DisplayBuf[page][x0], (uint8_t)(x1 - x0));
    }
}

void OLED_Clear(void)
{
    memset(OLED_DisplayBuf, 0x00, sizeof(OLED_DisplayBuf));
}

void OLED_ClearArea(int16_t X, int16_t Y, uint8_t Width, uint8_t Height)
{
    if (Width == 0 || Height == 0) {
        return;
    }

    int16_t x0 = (X < 0) ? 0 : X;
    int16_t x1 = (int16_t)(X + Width);
    if (x1 > 128) {
        x1 = 128;
    }
    int16_t y0 = (Y < 0) ? 0 : Y;
    int16_t y1 = (int16_t)(Y + Height);
    if (y1 > 64) {
        y1 = 64;
    }
    if (x0 >= x1 || y0 >= y1) {
        return;
    }

    for (uint8_t page = (uint8_t)(y0 / 8); page <= (uint8_t)((y1 - 1) / 8); page++) {
        memset(&OLED_DisplayBuf[page][x0], 0x00, (size_t)(x1 - x0));
    }
}

/* ===================== 绘图/字符 ===================== */

static void OLED_SetPixel(int16_t X, int16_t Y, uint8_t Value)
{
    if (X < 0 || X > 127 || Y < 0 || Y > 63) {
        return;
    }
    uint8_t mask = (uint8_t)(0x01 << (Y % 8));
    if (Value) {
        OLED_DisplayBuf[Y / 8][X] |= mask;
    } else {
        OLED_DisplayBuf[Y / 8][X] &= (uint8_t)~mask;
    }
}

void OLED_ShowImage(int16_t X, int16_t Y, uint8_t Width, uint8_t Height, const uint8_t *Image)
{
    if (Image == NULL) {
        return;
    }

    /* 字模格式：纵向 8 点、高位在下；字节下标 = (行 / 8) * Width + 列 */
    for (uint8_t j = 0; j < Height; j++) {
        for (uint8_t i = 0; i < Width; i++) {
            uint8_t bits = Image[(j / 8) * Width + i];
            OLED_SetPixel((int16_t)(X + i), (int16_t)(Y + j), (bits >> (j % 8)) & 0x01);
        }
    }
}

void OLED_ShowChar(int16_t X, int16_t Y, char Char, uint8_t FontSize)
{
    if (FontSize != OLED_8X16) {
        /* 本工程只用 8x16；6x8 字模未移植 */
        return;
    }

    if (Char < ' ' || Char > '~') {
        Char = '?';
    }

    OLED_ShowImage(X, Y, 8, 16, OLED_F8x16[(uint8_t)Char - (uint8_t)' ']);
}

void OLED_ShowString(int16_t X, int16_t Y, const char *String, uint8_t FontSize)
{
    if (String == NULL) {
        return;
    }

    int16_t x_offset = 0;
    while (*String != '\0') {
        OLED_ShowChar((int16_t)(X + x_offset), Y, *String, FontSize);
        x_offset = (int16_t)(x_offset + 8);
        if ((int16_t)(X + x_offset) >= 128) {
            break;
        }
        String++;
    }
}

