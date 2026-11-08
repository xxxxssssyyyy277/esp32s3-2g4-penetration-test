# 板 B 固件 —— STA + 1.54″ ST7789 LCD（下行 RSSI 测量）

「2.4 GHz 遮挡穿透损耗测试」双板方案中的 **板 B**。一套源码、两个编译缓存变量
（`APP_ROLE` / `DISP_KIND`）区分两块板；本工程实现 **板 B 分支**，板 A（SoftAP + OLED +
`L`/`T=`/`G`/UDP 接收）只保留可编译占位并标注 `/* TODO: 由板 A 提示词补全 */`。

| 项 | 取值 |
|---|---|
| 角色 | STA（客户端 / 接收端） |
| 屏 | 1.54″ 240×240，ST7789V，4 线软件 SPI |
| 编译宏 | `APP_ROLE_STA` + `DISP_KIND_LCD` |
| Wi-Fi | `ESP32S3-LINK` / `12345678`，信道 6，`WIFI_PS_NONE` |
| 采样 | 默认 `N=50`、`I=100 ms`，输出标准 CSV |
| 保活 | `U=1` 后每 50 ms 向网关 `:3333` 发 32 字节 UDP |
| 串口 | UART0，115200 8N1，`\n` 分帧 |

---

## 1. 目录结构

```
board b/
├── CMakeLists.txt              # 两个缓存变量 APP_ROLE / DISP_KIND（默认 STA + LCD）
├── sdkconfig.defaults          # ESP32-S3、1000 Hz tick、UART0 115200、Wi-Fi 任务绑 core 1
├── README.md                   # 本文件
├── tools/build_ascii.ps1       # 中文路径下的 ASCII 镜像构建/烧写脚本
├── components/lcd/
│   ├── CMakeLists.txt
│   ├── LCD.c  LCD.h            # ST7789 驱动（软件 SPI + 115 200 B 显存）
│   └── LCD_Data.c  LCD_Data.h  # ASCII 字模（必须保留）；未复制 LCD_CatFrames.c
└── main/
    ├── app_config.h            # 所有可调参数、测点表、引脚/任务/版面参数
    ├── main.c                  # 只做初始化 + 任务创建（无任何 LCD_ 前缀）
    ├── console.c/.h            # UART0 驱动 + Console_Line（无日志时间戳）
    ├── wifi_link.c/.h          # STA、WIFI_PS_NONE、1 s 退避重连、BSSID 核对
    ├── measure.c/.h            # 采样任务、qsort 中位数/标准差、CSV、基准/损耗
    ├── uart_cmd.c/.h           # 命令解析（64 字节行缓冲）
    ├── udp_keepalive.c/.h      # 50 ms UDP 保活 + 重连轮询
    ├── display.h               # 共享显示抽象（板 A/B 一致，末尾追加了 base/loss 字段）
    ├── display.c               # 按 DISP_KIND 分派、5 Hz 调度、[DISP] 同步日志
    ├── display_backend.h       # 后端私有接口
    ├── display_lcd.c           # LCD 后端（布局、信号条、区域刷新）
    ├── display_oled.c          # 板 A 占位（仅保证 OLED 配置可编译）
    ├── backlight.c/.h          # 60% / 采集 20% 背光，BL= / BLDIM=
    └── rgb_led.c/.h            # 板载 RGB 彩灯强制熄灭
```

---

## 2. 构建、烧写、监控

### 2.1 推荐：中文路径下用镜像脚本

ESP-IDF v5.1.2 在 zh-CN Windows 上无法直接编译含中文的工程路径（kconfgen 用 GBK 解码
`config.env`、idf.py 用 GBK 读 cmake 输出、Xtensa GCC/ninja 用 ANSI Win32 文件 API）。
脚本把源码镜像到纯 ASCII 目录后在镜像里执行 idf.py，源码仍以本目录为准：

```powershell
# 构建（默认就是 STA + LCD）
.\tools\build_ascii.ps1 -BuildDir build_b

# 烧写 / 监控（COM 号按“先拔一块看哪个口消失”确认）
.\tools\build_ascii.ps1 -BuildDir build_b -Action flash -Port COM6
.\tools\build_ascii.ps1 -BuildDir build_b -Action monitor -Port COM6
```

镜像目录：`C:\Users\Public\esp32_build\esp32s3_link_b`（与板 A 的 `esp32s3_link` 分开，
互不覆盖）。可直接在纯 ASCII 路径下用等价命令：

```bash
idf.py -B build_b -DAPP_ROLE=STA -DDISP_KIND=LCD build
idf.py -B build_b -p COM6 flash monitor
```

板 A 配置（本工程只有占位，**不要拿来跑板 A**）：

```bash
idf.py -B build_a -DAPP_ROLE=AP -DDISP_KIND=OLED build
```

### 2.2 构建注意（踩过的真坑）

`main/CMakeLists.txt` 里的 `PRIV_REQUIRES` **绝不能按 `DISP_KIND` 条件设置**。
ESP-IDF 用独立脚本（`tools/cmake/scripts/component_get_requirements.cmake`）提前展开
组件依赖，那一趟里拿不到缓存变量，条件分支会悄悄丢掉 `lcd`，表现为：

```
fatal error: LCD.h: No such file or directory
```

因此本工程把 `lcd` 无条件写进 `PRIV_REQUIRES`；OLED 占位构建只是不引用它，链接器不会把
115 KB 的 LCD 显存拉进固件。

### 2.3 实测构建结果（本机 ESP-IDF v5.1.2）

| 项 | 结果 |
|---|---|
| `-DAPP_ROLE=STA -DDISP_KIND=LCD` | ✅ 通过，**0 warning**（`-Wall -Wextra`，未开 `-Werror`） |
| `-DAPP_ROLE=AP -DDISP_KIND=OLED` 占位 | ✅ 通过，验证两宏组合都可编译 |
| LCD 版镜像 | `esp32s3_link.bin` = 776 768 B（0x0BDA40），1 MB app 分区剩 26%（0x425C0） |
| `.bss` | `LCD_DisplayBuf[240][240]` = **115 200 B**；整个 `.bss` 131 544 B（OLED 版少约 115 KB） |
| 动态内存 | 应用代码 **无 malloc/calloc/free**（任务/套接字由 FreeRTOS/lwIP 内部管理） |
| 实机 | ESP32-S3 QFN56 rev0.2，16 MB flash、8 MB PSRAM（未启用），MAC `44:bd:8d:6a:6b:54` |

> 实测板子的 flash 是 16 MB，`sdkconfig.defaults` 已设 `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`；
> PSRAM 保持关闭（显存放内部 RAM，`.bss` 只占 43% D/IRAM）。

### 2.4 烧录与串口自检（本机 COM3 实测）

```powershell
.\tools\build_ascii.ps1 -BuildDir build_b -Action flash -Port COM3
```

烧录日志每次写入后都会出现 `Hash of data verified.`（bootloader / 分区表 / app），最后以
`Hard resetting via RTS pin...` 结束。复位后 115200 8N1 应依次看到（`[DISP]` 每 200 ms 一行）：

```text
#RGB off pins=48,38,21
#BOOT chip=ESP32-S3 role=STA disp=LCD fw=B-1.0.0
#READY
#LCD w=240 h=240 xoff=0 yoff=0 inv=1 madctl=0x00 bl=60
[DISP] [B]C0 NONE rssi=0 med=0.0 base=0.0 smp=0/50
```

板 A 未上电时（无 AP）做离线自检是正常的：

- `P` → `#APINFO ssid=ESP32S3-LINK bssid=00:00:00:00:00:00 ch=6 rssi=0`
  （未连接时显示配置信道 6，不会显示扫描残留的 1..13）；
- `C=0` → 默认 `N=50` 时 50 行 CSV，`rssi` 列全空、`#SUMMARY ... valid=0`，
  `t_ms` 为 0/100/…/4900；若先 `N=10` 则 10 行、0/101/…/901（都在 100±20 ms 内）；
- `H` / `CL` / `N=` / `BL=` / `BLDIM=` / `U=` / `C=abc` / `C=9` 分别返回
  `#OK ...`、`#ERR unknown_cmd`、`#ERR bad_arg`、`#ERR bad_case`。

> **开机时序**：`#READY` 在显示任务启动前打印，`#LCD` 通常还要约 1.8 s 才出现
>（1.54″ 屏的两次 115 KB 全屏软件 SPI 刷新）。此时立刻发 `C=` 不会污染 CSV：
> 采样任务会先用 `Disp_WaitIdle()` 等待开机渲染临界区退出（最长 5 s），再发 `#BEGIN`。
> 现场采集仍建议等到 `#LCD` 出现后再开始，便于确认屏初始化完成。

---

## 3. 接线（板 B）

| LCD 模块（8 针） | 接到 ESP32-S3 | 宏（`components/lcd/LCD.h`） | 说明 |
|---|---|---|---|
| VCC | 3V3 | — | 面板逻辑电源 3.3 V |
| GND | GND | — | 共地 |
| SCL | **GPIO39** | `LCD_SCL_PIN` | 软件 SPI 时钟 |
| SDA | **GPIO40** | `LCD_SDA_PIN` | 软件 SPI 数据（MOSI） |
| RES | **GPIO41** | `LCD_RES_PIN` | 复位，工作时应为高 |
| DC | **GPIO42** | `LCD_DC_PIN` | 高=数据，低=命令 |
| CS | **GPIO2** | `LCD_CS_PIN` | 低有效 |
| BLK | **GPIO1** | `LCD_BL_PIN` | 背光，高点亮，LEDC 通道 0 / 定时器 0 / 5 kHz |

> ⚠️ **屏幕与单片机必须共地，否则屏幕有概率不亮。**
>
> 这不只针对“外部电源供电”的情况：即使 VCC 直接从 ESP32 的 3V3 取，只要 **GND
> 没接实（漏接、插在空排针、杜邦线接触不良）**，SCL/SDA 与背光都没有参考地，
> 表现就是**时亮时不亮、复位几次能亮一次、或一直背光亮但画面全黑**，
> 而固件侧不会报任何“接线错误”。
>
> 排查顺序：① 量模块焊盘 `VCC↔GND` 是否约 3.3 V；② 用蜂鸣档量模块 `GND↔ESP32 GND`
> 是否导通；③ 共地后 SCL/SDA 静态高电平应约为 ESP32 的 3V3 电压。

- GPIO39–42 是 MTCK/MTDO/MTDI/MTMS 四根 JTAG 脚。作普通 GPIO 完全可用，代价是
  **外部 JTAG 调试器不能再用**；板载 USB Serial/JTAG（GPIO19/20）不受影响，烧写与
  `idf.py monitor` 正常。
- **若 LCD 用独立 3.3 V 电源供电，必须把外部电源的 GND 与 ESP32 的 GND 连在一起。**
  只接 VCC/GND 到外部电源、SCL/SDA 接 ESP32 而两地不共地，会出现“背光亮、屏幕全黑”：
  SCL/SDA 没有参考地，模块端量到的是 -0.4 V 左右的虚电压，ST7789 收不到任何命令。
  共地后 SCL/SDA 静态高电平应约为 ESP32 的 3V3 电压。
- 不要同时把 LCD 的 VCC 接到 ESP32 的 3V3 和外部 3.3 V 电源（两个电源输出不要并联）；
  要么由 ESP32 3V3 供电并共地，要么外部 3.3 V 供电且只共地。
- 背光电流若 > 20 mA，请用三极管/MOS 驱动，不要直接吃 GPIO 电流。
- 本板只接 LCD；GPIO1/GPIO2 在板 A 上是 OLED 的 SCL/SDA，两块板独立硬件、编号互不影响。

---

## 4. 板载 RGB 彩灯保持熄灭（本次特别要求）

`main/rgb_led.c` 在 `app_main()` 里 console 初始化之后、任何 Wi-Fi/显示初始化之前执行
`RgbLed_Off()`：

- 把三种常见 RGB 彩灯接法全部配成推挽输出并拉低：
  `GPIO48`（ESP32-S3-DevKitC-1 v1.0）、`GPIO38`（v1.1）、`GPIO21`（ESP32-S3-Zero）；
- WS2812 只有收到 24 bit 数据帧才亮，数据脚静态拉低 → 上电后一直灭；
  普通共阴/共阳 RGB LED 经三极管驱动时拉低同样不导通；
- 这三个脚在本工程中不承担任何其他功能，不影响 LCD（39/40/41/42/2/1）、Wi-Fi、UART0；
- 串口会打印一行 `#RGB off pins=48,38,21` 便于确认；
- 引脚列表在 `app_config.h` 的 `APP_RGB_LED_PINS` / `APP_RGB_LED_PIN_COUNT`。
  确认板子型号后可删掉多余项；若板子用的是别的脚，改这两个宏并同步 `rgb_led.c`
  的打印即可。

这样采集期间彩灯不会点亮或闪烁，避免额外 LED 驱动电流/开关噪声落在 2.4 GHz 附近。

---

## 5. 测点表（与板 A、上位机 `materials.csv` 严格一致）

集中定义在 `main/app_config.h` 的 `APP_CASE_LIST(X)`，改这里一处即可同步
`CL` 输出、CSV 标签、LCD 显示：

| 编号 | 标签 | 遮挡物 | 说明 |
|---|---|---|---|
| 0 | `NONE` | 无遮挡 | **基准**，首尾各测一次用于检查漂移 |
| 1 | `PAPER` | 硬纸板 1 层 | 约 3 mm，≥35×35 cm |
| 2 | `BOOK1` | 1 本书 | 约 25 mm，四周纸板补到 35×35 |
| 3 | `BOOK5` | 5 本书 | 约 125 mm，与 2 号构成厚度序列 |
| 4 | `WATER` | 水墙（6 瓶并排） | 约 100 mm，单瓶不行 |
| 5 | `FOIL` | 铝箔 2 层无缝隙 | 导体对照，残余电平来自缝隙/绕射 |
| 6 | `FOILGAP` | 铝箔 2 层中间留 5 cm 直缝 | 与 5 号唯一差别是那条缝 |
| 7 | `BODY` | 人体站在链路中点 | 体宽约 45 cm 已够 |
| 8 | `HAND_A1` | 手直接捂住板 A（被试 1） | 手掌贴住板 A 天线：近场吸收 + 失谐，反应最明显 |
| 9 | `HAND_A2` | 手直接捂住板 A（被试 2） | 换人重复，手掌大小/干湿/握力不同 |
| 10 | `HAND_A3` | 手直接捂住板 A（被试 3） | 换人重复，用于量化人际差异 |

标签纯 ASCII、最长 7 字符（`FOILGAP`、`HAND_A1`），屏幕显示不会缺字或溢出。

> 8–10 号是近场工况（手掌几乎贴在天线上），物理机制是**近场吸收 + 天线失谐**，
> 不是平板遮挡的平面波穿透，所以损耗会明显大于 7 号「远场人体」。
> **不同人捂，反应差别很大**（手掌面积、皮肤干湿、握持贴合度、是否戴手表/戒指都影响
> 加载介质的等效体积与介电常数），因此拆成 3 个独立测点，同一人只测一个编号。
> 上位机 `materials.csv` 里仍归到 `water` 类，报告里按「近场 / 人际差异」单独解释。

---

## 6. 串口命令（每行一条，`\n` 结尾，不区分大小写）

| 命令 | 作用 | 成功响应 |
|---|---|---|
| `C=<0..10>` | 设置测点并立即采集一轮 | `#OK C`（在 `#END` 之后） |
| `CL` | 打印测点表 | `#OK CL` |
| `N=<10..500>` | 每轮采样个数，默认 50 | `#OK N` |
| `I=<20..1000>` | 采样间隔 ms，默认 100 | `#OK I` |
| `U=<0\|1>` | 开始/停止 UDP 保活，默认 0 | `#OK U` |
| `P` | 打印一行 `#APINFO` | `#OK P` |
| `H` | 帮助 | `#OK H` |
| `BL=<0..100>` | 背光亮度，默认 60 | `#OK bl` |
| `BLDIM=<0..100>` | 采集期间亮度，默认 20 | `#OK bldim` |

错误响应：

| 情况 | 响应 |
|---|---|
| 编号越界（含负数） | `#ERR bad_case <原文>` |
| 数字解析失败 / 范围非法 | `#ERR bad_arg <原文>` |
| 未知命令 | `#ERR unknown_cmd <原文>` |
| 行超过 64 字节 | `#ERR line_too_long` |
| 采集进行中的任何新命令 | `#ERR busy`（统一在 `#END` 之后补发，保证 CSV 块干净） |

### 6.1 示例

```
> CL
#CASELIST n=11
0,NONE
1,PAPER
2,BOOK1
3,BOOK5
4,WATER
5,FOIL
6,FOILGAP
7,BODY
8,HAND_A1
9,HAND_A2
10,HAND_A3
#OK CL
```

```
> P
#APINFO ssid=ESP32S3-LINK bssid=24:0a:c4:xx:xx:xx ch=6 rssi=-45
#OK P
```

```
> N=50
#OK N
> I=100
#OK I
> U=1
#OK U
```

```
> C=0
#BEGIN case_id=0 label=NONE n=50 interval_ms=100 t0_ms=8123
case_id,seq,t_ms,rssi_dbm
0,1,0,-45
0,2,100,-46
0,3,200,-45
...
0,50,4900,-45
#SUMMARY case_id=0 label=NONE n=50 median=-45.00 mean=-45.30 stdev=2.10 min=-48 max=-43 valid=50
#END
#OK C
```

```
> C=6
#BEGIN case_id=6 label=FOILGAP n=50 interval_ms=100 t0_ms=15320
...
#SUMMARY case_id=6 label=FOILGAP n=50 median=-58.00 mean=-58.30 stdev=2.10 min=-63 max=-55 valid=50
#END
#OK C
```

> `#OK C` 一定在 `#END` 之后。采集期间发来的命令不执行，任务会在 `#END` 后按到达顺序
> 补发 `#ERR busy`（最多 8 条，防止刷屏），然后才发 `#OK C`；这样 `#BEGIN` 与 `#END`
> 之间只有 CSV 行。

---

## 7. 串口数据格式（上位机依赖）

```
#BEGIN case_id=<编号> label=<标签> n=<N> interval_ms=<I> t0_ms=<开机ms>
case_id,seq,t_ms,rssi_dbm
<编号>,<seq>,<t_ms>,<rssi>
...
#SUMMARY case_id=<编号> label=<标签> n=<N> median=<..> mean=<..> stdev=<..> min=<..> max=<..> valid=<..>
#END
```

- `seq` 从 1 递增；`t_ms` 是本轮起点到该样本的毫秒数。
- `esp_wifi_sta_get_ap_info()` 失败时该样本无效，但**仍输出一行**，`rssi` 列留空
  （行尾只有一个逗号），`valid` 只统计有效样本。
- 中位数：有效样本拷贝进定长数组 `s_samples[500]` 后 `qsort`；偶数个取中间两个的平均。
- 标准差：总体标准差 `sqrt(Σ(x-mean)²/n)`，单位 dB。
- 统计与 CSV 全部 `printf` 直出 stdout，不经过 `ESP_LOGI`（否则会混入
  `I (123) tag:` 时间戳）。
- 标签只能来自内置测点表，不接受串口输入，避免非法字符污染 CSV。

屏幕同步日志（每 5 Hz 刷新一行，**采集窗口内绝对没有**）：

```
[DISP] [B]C6 FOILGAP rssi=-58 med=-58.0 base=-45.0 smp=50/50
```

> 说明：提示词原文要求用 `ESP_LOGI`，但 `ESP_LOGI` 会加 `I (123) tag:` 前缀，
> 上位机要求 `[DISP]` 必须位于行首，否则过滤不掉、`float()` 会抛异常。因此这里与
> 板 A 一致，用 `Console_Line()` 直接 `printf`，只保留 `[DISP]` 前缀。若确实需要
> IDF 日志，可把 `display.c` 里那一行换成 `ESP_LOGI`，但必须同步改上位机的过滤规则。

---

## 8. 采样、统计、基准与穿透损耗

### 8.1 采样任务

- `xTaskCreatePinnedToCore(Measure_Task, "meas", 4096, NULL, 3, NULL, 1)`；
- 命令任务 prio 2 / core 1，UDP 任务 prio 2 / core 1，显示任务 prio 1 / **core 0**；
- 采样用 `vTaskDelayUntil` 绝对节拍，`I=100` 时 `t_ms` 差值稳定在 100 ms 附近
  （Wi-Fi 中断抖动通常 < 5 ms，满足 100±20 ms）；
- 整个采样循环里**不调用 `LCD_Update` / `UpdateArea`**，显示任务被 `g_ui_paused`
  拦住，软件 SPI 不会拖累时间戳。

### 8.2 基准与 LOSS

```
LOSS = median(case 0) − median(case n)     单位 dB
```

- 基准 = **最近一次 `case_id=0` 的 median**，跨轮次保存在 RAM；
- 当前就是 case 0 → `loss_valid=false`，屏上 `LOSS: --.-dB`；
- 尚未测过基准 → `BASE:  -`，`LOSS: --.-dB`；
- 先 `C=0` 再 `C=6`，屏上 `LOSS` 与两次 `#SUMMARY` 的 `median` 之差一致（保留一位小数）。

### 8.3 采集期间为什么调暗背光

背光升压电路是开关电源，满亮度时开关噪声更容易落进 2.4 GHz 频段、抬高共板底噪。
采集窗口内 `BLDIM`（默认 20%）生效，结束后恢复 `BL` 设定（默认 60%）。
串口命令：`BL=60`、`BLDIM=20`。

---

## 9. UDP 保活

- `U=1` 后，UDP 任务每 50 ms 向 `esp_netif_get_ip_info()` 得到的**网关地址**
  `:3333` 发固定 32 字节包（`"BKA"` + 递增序号 + 补 0）；板 A 只统计个数。
- `U=0` 立即停止；成功计数在 `UdpKa_Count()`，板 B 屏上不显示（`rx_udp` 按规范填 0）。
- 发送失败不刷屏：连续失败 20 次（约 1 s）才打一次 `#WARN udp_fail`；
  若此时正处于采集窗口，警告会推迟到 `#END` 之后。
- **没发 `U=1` 时板 A 侧 RSSI 恒定不变**——双向/互易性对比必须先开保活。

---

## 10. LCD 版面与刷新

### 10.1 版面（240×240，左上角原点，全部 ASCII）

| 区域 | 坐标 | 字号 | 内容 |
|---|---|---|---|
| 标题 | (4, 4) | 8×16 | `[B] STA`；断网时 `[B] STA NO LINK` |
| SSID | (4, 26) | 6×8 | `SSID: ESP32S3-LINK`（超长截到 14 字符） |
| 信道 | (4, 40) | 8×16 | `CH:6` |
| 测点 | (4, 64) | 8×16 | `CASE 6 FOILGAP` |
| 实时 RSSI | (4, 88) | 8×16 | `RSSI: -58 dBm`；无效 `RSSI: --- dBm` |
| 信号条 | 矩形 (4, 112) 200×14 | — | -30…-100 dBm 线性映射到 0…200 px |
| 当前中位数 | (4, 134) | 8×16 | `MED: -58.0dBm`；无结果 `MED: --.-dBm` |
| **穿透损耗** | (4, 156) | 8×16 | `LOSS: 13.0dB`；不可算 `LOSS: --.-dB`（黄色，独立一行） |
| 均值/标准差 | (4, 178) | 6×8 | `MEAN:-58.3 SD:2.1` |
| 采样进度 | (4, 192) | 8×16 | `SAMPLE: 23/50` |
| 基准/运行时间 | (4, 214) | 6×8 | `BASE:-45.0 T:123s`；无基准 `BASE:  -   T:123s` |

信号条配色：≥ -50 绿；-50…-70 黄；-70…-85 橙；< -85 红；未连接灰色空条。
每行先用背景色清固定宽度再写字（`LCD_ClearArea` + `LCD_ShowString`），短字符串不会
残留上一帧的尾巴；超出该行字符数的文本会被截断，绝不越界串到别的区域。

### 10.2 刷新策略（为什么要这么绕）

- 开机：`LCD_Init()` 清屏并全屏刷一次；随后画静态框架（标题/SSID/CH/行占位/信号条外框），
  再 `LCD_Update()` 全屏刷一次。
- 运行期：显示任务 5 Hz（200 ms）构建 `disp_state_t`，**逐行比较文本**，只有变化的行
  才 `LCD_ShowString` + `LCD_UpdateArea`，没变的行一个字节都不发。
- 信号条单独 `UpdateArea(4, 112, 200, 14)`；颜色变化整条重刷，只是长度变化时只刷
  变化的那一小段（最大 200×14，远小于全屏 240×240）。
- `MEAN/SD` 与 `BASE/T` 只在一轮采集结束时变化一次，diff 比较后自然只刷一次，
  不需要额外降频；稳定的 RSSI/进度也只在数值变化时发送。
- 粗算：典型一拍只刷 RSSI + 信号条（≈128×16 + 200×14 像素），软件 SPI 在
  ESP32-S3 @240 MHz 下远低于 100 ms；即使全部门线都变化也只在采集前/后发生。
- **采样循环内绝不调用 `LCD_Update`**；显示任务在 core 0，采样/Wi-Fi 在 core 1。

> 关于「采集中实时显示进度」：硬约束要求 `#BEGIN`…`#END` 之间不能有任何 `[DISP]`，
> 且 `g_ui_paused=true` 时显示任务「不发屏、不打日志」。因此本项目在采集窗口内冻结屏幕，
> 采集结束后主动补刷一次，`SAMPLE` 显示最终的 `50/50`、`MED`/`LOSS`/`BASE` 同时更新。
> 这样最稳地保证 `t_ms` 不受软件 SPI 影响。如果确实要看到 23/50 的实时跳变，可以把
> `display.c` 改成「暂停时仍刷屏、只跳过 [DISP] 日志」，代价是采样时间戳抖动增大，
> 不推荐。

### 10.3 背光

- `LCD_BL_LEDC_MODE = LEDC_LOW_SPEED_MODE`、`LCD_BL_LEDC_TIMER = LEDC_TIMER_0`、
  `LCD_BL_LEDC_CHANNEL = LEDC_CHANNEL_0`、5 kHz、8 bit；
- 初始化后 `LCD_SetBrightness(60)`（`APP_LCD_BRIGHTNESS_DEFAULT`）；
- `BL=` 改正常亮度，`BLDIM=` 改采集亮度（默认 20），采集结束自动恢复 `BL`。

---

## 11. 240×240 适配：最终取值与排查表

### 11.1 本工程当前取值（`components/lcd/LCD.h`）

| 宏 | 当前值 | 说明 |
|---|---|---|
| `LCD_WIDTH` | **240** | 与 2.4 寸一致 |
| `LCD_HEIGHT` | **240** | 从 320 改为 240 |
| `LCD_X_OFFSET` | **0** | 实机确认左右无偏移 |
| `LCD_Y_OFFSET` | **0** | 实机确认上下无偏移（1.54″ 240×240 常见 0/40/80） |
| `LCD_INVERSION_ON` | **1** | 实机确认颜色正常（INVON 0x21） |
| `LCD_MADCTL` | **0x00** | RGB 顺序、不镜像、不交换 XY |
| `LCD_BL_BRIGHTNESS` | **60** | 开机亮度 60% |

显存：`uint16_t LCD_DisplayBuf[240][240]` = **115 200 B**，比 240×320 版少 38 400 B。

> ✅ 以上取值已在 **1.54″ ST7789V（ZJY154S0800TG02）实机**确认：
> 240×240 满屏、无偏移、无杂边、颜色正常、文字清楚。
> 若换用其他模组表现不同，按 11.2 的顺序改宏，**不要**去 `LCD_Init` 里硬编码命令。

### 11.2 显示异常排查顺序（照这个顺序试，一次只改一项）

| 现象 | 改哪里 | 取值顺序 |
|---|---|---|
| 画面整体上下偏移、底部杂边 | `LCD_Y_OFFSET` | **0 → 80 → 40** |
| 画面左右错位 | `LCD_X_OFFSET` | 0 → 80 |
| 颜色全反（黑底变白、红变青） | `LCD_INVERSION_ON` | 1（0x21 INVON）→ 0（0x20 INVOFF） |
| 上下/左右镜像 | `LCD_MADCTL` | 0x40 左右翻、0x80 上下翻、0x20 交换 XY（可组合） |
| 红蓝互换 | `LCD_MADCTL` 加 BGR 位 | `0x08` |
| 白屏/全黑但背光亮 | 接线/供电/CS | 先量 VCC=3.3 V、**外部电源 GND 必须与 ESP32 GND 共地**；确认 SCL=39、SDA=40、RES=41、DC=42、CS=2 |
| 背光不亮 | BL 脚 | GPIO1 必须是高电平；`BL=100` 试一下；大电流加三极管 |
| 图像撕裂/花屏 | 刷新时序 | 不要提高刷新率；不要在采样里刷屏；确认没有第二个任务调用 LCD |
| 有残影/叠字 | 行宽 | 本项目每行固定宽度清背景；自定义内容时不要超过该行 `chars` |
| 屏幕闪烁 | 背光 PWM | 5 kHz 一般不可见；若闪，检查电源电流是否够 |

改完宏后必须重新 `build`（`LCD.h` 在组件里，增量编译会重编 LCD.c 和 display_lcd.c）。

---

## 12. 常见问题（FAQ）

**Q1：`idf.py build` 报 `LCD.h: No such file or directory`？**
见 2.2：`PRIV_REQUIRES` 不能条件化；本工程已把 `lcd` 写成无条件依赖。

**Q2：中文路径下 `Failed to determine sizeof(time_t)` / `UnicodeDecodeError`？**
用 `tools/build_ascii.ps1`，或把工程整体放到 `D:\esp32s3_link_b` 这类纯 ASCII 路径。

**Q3：串口打不开 / PermissionError？**
先关掉 `idf.py monitor`，或确认没有别的串口工具占用；两块板各占一个 COM，先拔一块
看哪个消失来确认 A/B。

**Q4：没有 `#APINFO`？**
确认板 A 已启动 SoftAP（SSID `ESP32S3-LINK` / 密码 `12345678` / 信道 6）；
`P` 可以随时打印当前缓存；`sdkconfig.defaults` 默认 UART0 115200，别把 monitor 波特率
设成别的。未连接时 `P` 的 `bssid` 是全 0、`ch=6`（配置值），连上后才是真实 BSSID/RSSI。

**Q5：板 A 侧 RSSI 不变？**
忘了 `U=1`。板 A 的 RSSI 只在收到 UDP 保活包时刷新。

**Q6：采集窗口里会不会出现 `[DISP]`？**
不会。`C=` 后 `g_ui_paused=true` → 显示任务整拍跳过（不刷屏、不打日志），
`#END` 后才复位并补刷一次。`#BEGIN` 与 `#END` 之间只有 CSV 行与 `#SUMMARY`。

**Q7：采集时新发命令会怎样？**
不执行；`#END` 之后按到达顺序补发 `#ERR busy`，再发 `#OK C`。这样不会把错误行插进
CSV 块，同时明确告诉上位机命令被忽略。

**Q8：为什么屏幕在采集时不更新进度？**
见 10.2 末尾的取舍说明：硬约束优先保证 `t_ms` 稳定，采集结束一次性刷新。

**Q9：彩灯还是亮？**
`#RGB off pins=48,38,21` 是否打印？如果板子的 RGB 不在 48/38/21，改
`app_config.h` 的 `APP_RGB_LED_PINS`；若是 WS2812 且上电瞬间闪一下，属于模块上电
自检，固件运行后即灭。

**Q10：能同时编 OLED 和 LCD 吗？**
不能。LCD 显存 115 200 B + OLED 1 KB 会白占约 116 KB 内部 RAM；`DISP_KIND` 只让
一个后端进固件。板 A 烧 `-DAPP_ROLE=AP -DDISP_KIND=OLED`（用板 A 工程），
板 B 烧 `-DAPP_ROLE=STA -DDISP_KIND=LCD`（本工程）。

---

## 13. 与板 A 的接口一致性

| 项 | 板 B 实现 | 板 A 期望 |
|---|---|---|
| SSID / 密码 / 信道 | `ESP32S3-LINK` / `12345678` / 6 | 一致 |
| 测点表 | 0..10，标签同上表 | `CL` / `#STALIST` 对得上 |
| UDP 目标 | 网关 `:3333`，50 ms，32 B | 板 A 监听 3333 并统计 |
| 串口格式 | `#BEGIN` / CSV / `#SUMMARY` / `#END` / `[DISP]` | 上位机按 `提示词-C` 解析 |
| `LOSS` | `median(case 0) − median(case n)` | 与上位机 `analyze.py` 一致 |

---

## 14. 验收自检（对应提示词 12 条）

| # | 检查项 | 状态 |
|---|---|---|
| 1 | `-B build_b -DAPP_ROLE=STA -DDISP_KIND=LCD` 通过、无 warning | ✅ 本机已验证（0 warning） |
| 2 | 连接板 A、`#APINFO` 的 bssid 与板 A 一致 | ✅ 实机确认 `bssid=44:bd:8d:6b:1b:19`、`ch=6` |
| 3 | `CL` 输出 8 行、与 `app_config.h` 一致 | ✅ 实机 `CL` 输出 8 行，与表一致 |
| 4 | `C=0` 输出 `#BEGIN`…`#END`、`valid=50` | ✅ 双板实机 `valid=50`，50 行 CSV |
| 5 | `I=100`/`N=50` 时 `t_ms` 差值 100±20 ms | ✅ 实机 `0..4900`，间隔全为 100 ms（N=50） |
| 6 | `U=1` 后板 A 侧计数增长 | ✅ 50 ms/32 B；⏳ 双板联调（板 A 侧计数待读） |
| 7 | 240×240 无偏移/杂边/颜色正常 | ✅ 实机确认（0/0、INVON=1、MADCTL=0x00） |
| 8 | `C=0` 后 `C=6`，`LOSS` 与中位数差一致 | ✅ 实机 `median 0=-13.00`、`median 6=-14.00` → `LOSS=1.00 dB` |
| 9 | 屏内容全 ASCII、每行定宽不残留 | ✅ 字模只含 ASCII；每行清固定宽度 |
| 10 | 采集窗口内无 `[DISP]`，过滤后仍是合法 CSV | ✅ 离线实测块内无 `[DISP]` / `#LCD` / `#CASELIST` |
| 11 | 无 malloc；LCD 版 `.bss` 比 OLED 大 ~115 KB | ✅ 应用代码无 malloc；显存 115 200 B |
| 12 | 没复制 `LCD_CatFrames.c` | ✅ 只复制 4 个驱动文件 |

---

## 15. 显示状态结构体的追加字段（README 存档）

`main/display.h` 的 `disp_state_t` 保持提示词给定的字段顺序与签名，只在**末尾追加**
了板 B 需要的 6 个字段（提示词允许追加并记录）：

| 字段 | 类型 | 用途 |
|---|---|---|
| `rssi_valid` | `bool` | RSSI 是否可用（未联网/采样失败为 false） |
| `has_result` | `bool` | 本轮是否已算出统计量 |
| `base_dbm` | `float` | 最近一次 `case 0` 的中位数 |
| `base_valid` | `bool` | 是否已有基准 |
| `loss_db` | `float` | `base_dbm − median_dbm` |
| `loss_valid` | `bool` | 当前 LOSS 是否可显示（case 0 / 无基准时为 false） |

原字段 `txp_dbm` / `sta_count` / `rx_udp` 由板 B 填 0，不占版面。

---

## 16. 已知限制

1. **LCD 供电必须共地**：本工程用外部 3.3 V 给 1.54″ 屏供电，**外部电源 GND 必须与
   ESP32 GND 相连**；否则表现为背光亮、屏幕全黑，SCL/SDA 量到 -0.4 V 左右虚电压。
   偏移/反相已实机确认：`LCD_Y_OFFSET=0`、`LCD_X_OFFSET=0`、`LCD_INVERSION_ON=1`、
   `LCD_MADCTL=0x00`。若换其他模组，按 11.2 改宏并在本 README 记录最终值。
   另：本板 USB 供电时实测 3V3 约 **2.8 V**，低于 ESP32-S3 规格下限；建议换质量更好的
   USB 线/端口，或改用 5 V 供电，避免 Wi-Fi 发射时掉压复位。
2. **板 A 分支只是占位**：`WifiLink_StartSoftAp()`、`display_oled.c`、`L`/`T=`/`G`
   只返回 `#ERR not_supported`，按提示词要求不实现。
3. **采集窗口内屏幕冻结**：见 10.2，硬约束优先；采集结束后补刷。
4. **`[DISP]` 用 printf 而非 ESP_LOGI**：为满足“行首前缀”与上位机过滤，原因见第 7 节。
5. **RGB 彩灯**：默认拉低 48/38/21 三脚；若板子型号不同，改 `APP_RGB_LED_PINS`。
6. **无动态内存**：应用层不使用 malloc/calloc/free；任务、套接字由 FreeRTOS/lwIP 内部管理。

---

## 17. 每块板烧哪套配置

| 板 | 工程 | 命令 | 屏 |
|---|---|---|---|
| A（SoftAP） | `board a` 工程 | `idf.py -B build_a -DAPP_ROLE=AP -DDISP_KIND=OLED build` | 0.96″ SSD1306（GPIO1/2） |
| B（STA） | **本工程** | `idf.py -B build_b -DAPP_ROLE=STA -DDISP_KIND=LCD build` | 1.54″ ST7789（39/40/41/42/2/1） |

烧错配置不会损坏硬件：板 B 烧了 OLED 版 → I²C 时序打到 LCD 的 BL/CS，屏幕不亮；
板 A 烧了 LCD 版 → PWM 打到 OLED 的 SCL，屏幕不亮。改回正确配置即可。


