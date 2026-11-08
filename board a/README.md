# 2.4 GHz 遮挡穿透损耗测试 —— 双板固件（ESP32-S3）

一套代码，两个编译期开关：

| 宏 | 取值 | 含义 |
|---|---|---|
| `APP_ROLE` | `AP` / `STA` | 板 A（SoftAP，发信号 + 读上行 RSSI）/ 板 B（STA，测下行 RSSI） |
| `DISP_KIND` | `OLED` / `LCD` | 0.96″ SSD1306（软件 I2C）/ 1.54″ ST7789 |

本次实现 **板 A 全功能**；板 B 的部分（STA 模式、`C=` 采样、LCD 后端、UDP 发送）保留了
可编译的占位，并标注 `/* TODO: 由板 B 提示词补全 */`。

## 构建

```bash
# 板 A
idf.py -B build_a -DAPP_ROLE=AP -DDISP_KIND=OLED build

# 板 B（占位，后续由板 B 提示词补全）
idf.py -B build_b -DAPP_ROLE=STA -DDISP_KIND=LCD build

# 板 A 诊断版（把引脚两两扫描、速度扫描、「上拉指纹」编进去，屏不亮时用）
idf.py -B build_a_scan -DAPP_ROLE=AP -DDISP_KIND=OLED -DOLED_SCAN_ALL=1 build
```

两个 build 目录互不干扰，`sdkconfig` 也在各自 build 目录里生成；工程根目录的
`sdkconfig.defaults` 只放公共默认项（目标芯片、115200 波特率、1 ms tick 等）。

> **工程路径必须是纯 ASCII。** ESP-IDF v5.1.2 在中文 Windows 上有三处会挂：
> kconfgen 用 GBK 解码 `config.env`、`idf.py` 用 GBK 读 CMake 输出、
> Xtensa GCC / ninja 用 ANSI Win32 接口打不开中文路径。
> 本工程在 `D:\作业相关\...\board a` 下直接执行上面的命令会在
> `Failed to determine sizeof(time_t)` 或 `UnicodeDecodeError` 处停下，
> 与本工程代码无关。

两条可用做法：

1. 把工程整体移到纯英文路径（如 `D:\esp32s3_link`）后再执行上面的命令；
2. 留在当前位置，用随工程提供的中转脚本（ASCII 路径镜像 + 构建）：

```powershell
# 板 A 构建 / 板 B 构建
.\tools\build_ascii.ps1 -Role AP  -Disp OLED -BuildDir build_a
.\tools\build_ascii.ps1 -Role STA -Disp LCD  -BuildDir build_b

# 板 A 烧写到 COM3（脚本会先镜像再 idf.py flash）
# 串口号按设备管理器里的实际值填：同一块板换 USB 口后会变成 COM6 之类
.\tools\build_ascii.ps1 -Role AP -Disp OLED -BuildDir build_a -Action flash -Port COM3

# 只看串口（115200），不会重新编译或烧写
.\tools\build_ascii.ps1 -Role AP -Disp OLED -BuildDir build_a -Port COM3 -Action monitor
```

脚本把源码（不含 `build_*`、`sdkconfig`）镜像到
`C:\Users\Public\esp32_build\esp32s3_link`，在那里执行等价的 `idf.py` 命令，
产物留在镜像目录里；源码仍以本目录为准（改完直接再跑一次即可）。

## 板 A 说明（SoftAP + OLED）

### 接线

| OLED 模块 | 接到 ESP32-S3 | 说明 |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | **必须接，且与 ESP32 共地** |
| SCK / SCL | **GPIO1** | 模块丝印两种叫法，同一根时钟线 |
| SDA | **GPIO2** | |

> ⚠️ **屏幕与单片机必须共地，否则屏幕有概率不亮。**
>
> 只把 SCL/SDA 接到 GPIO、而模块的 GND 没接到 ESP32 的 GND（或接触不良），
> 模块自带的 4.7 kΩ 上拉就没有参考地，总线电平浮空 —— 表现为**时亮时不亮、
> 复位几次能亮一次、或干脆一直黑屏**，而固件侧看不出任何“接线错误”。
> 用外部电源给 OLED 供电时同理：**外部电源的 GND 必须与 ESP32 的 GND 连在一起。**
>
> 排查时先量模块焊盘上 `VCC↔GND` 的电压，再量 `GND↔ESP32 GND` 是否导通 ——
> 这两步能覆盖绝大多数“屏不亮”。固件也会打印 `#OLED pullup none` / `pu=00`
> 提示「模块没供电或没共地」，见下文《屏不亮怎么查》。

模块一般自带 4.7 kΩ 上拉；若不带，SCL/SDA 各加一只 4.7 kΩ 到 3.3 V。
固件里同时开了 ESP32-S3 的内部弱上拉（约 45 kΩ）兜底，它只够让总线在空闲时保持高电平，
真正跑 I2C 还是靠模块上的 4.7 kΩ。

不要用 GPIO37（Octal PSRAM 的 DQS/DM）、GPIO38（FSPIWP）、GPIO9–14（SPI2 的 IOMUX 组）。

引脚宏在 `components/oled/OLED.h`（组件不能包含 `main/` 头文件），可用
`idf.py -DOLED_SCL_PIN=... -DOLED_SDA_PIN=... -DOLED_I2C_DELAY_US=...` 覆盖；
用中转脚本时这样传：

```powershell
.\tools\build_ascii.ps1 -Role AP -Disp OLED -BuildDir build_a -Action flash -Port COM3 `
    -SclPin 4 -SdaPin 5
```

### 屏不亮怎么查（固件自己会报）

`Disp_Init()` 会先探测地址，再**无条件**把整段初始化序列发出去（和江协 V2.0 参考实现
一样，不把「读到 ACK」当作初始化前提），最后把结果打在串口上：

| 串口行 | 含义 | 处理 |
|---|---|---|
| `#OLED addr=0x78 spd=1us scl=1 sda=2 ack=27` | 总线通，屏在 0x3C，收到 27 个 ACK | 正常，屏该亮了 |
| `#OLED addr=0x78 spd=1us scl=2 sda=1 ack=27` | 屏在 0x3C，但 **SCL/SDA 插反了**，固件已自动换过来 | 能用；建议按丝印改成 SCL→GPIO1、SDA→GPIO2 |
| `#OLED addr=0x7A ...` | 屏焊的是 0x3D | 正常，固件已自动改用 0x7A |
| `#OLED addr=0x78 spd=5us` / `20us` | 上拉偏弱、线偏长，固件自动降速后通了 | 最好还是补 4.7 kΩ 上拉 |
| `#OLED err=nack scl=1 sda=1 scl_pin=1 sda_pin=2 ...` | 两个地址都没读到 ACK（初始化序列仍然已经盲发出去） | 见下方排查表 |
| `#OLED err=nack scl=1 sda=1 pu=00` | 空闲高电平，但**内部下拉后也读到 0**，说明这两个脚上没有任何外部上拉 | 屏的 VCC 没真正接到 3V3（模块自带的 4.7 kΩ 上拉就挂在 VCC 上）、GND 没接、线插错排针或没插到底；0.91″ 便宜模块本身可能无上拉，此项仅供参考 |
| `#OLED err=nack scl=0` 或 `sda=0` | 线被拉死 | 模块没供电（VCC/GND 反接）、SDA 对地短路 |
| `#OLED scan scl=.. sda=.. addr=0x..` | 诊断版固件扫到了屏 | 用 `-SclPin/-SdaPin` 按这个组合重新编译即可 |
| `#OLED sweep 2000us noack` | 诊断版：总线上把位延时从 1 µs 一路放到 2 ms（250 Hz），每档 ping 一次 | **全部** noack ⇒ SCL/SDA 在电气上没接到这两颗脚（不是速度问题，加慢也没用） |
| `#OLED sweep 100us ack addr=0x78` | 只有慢速档才应答 | 线接对了，但没有板载上拉 / 杜邦线太长，补 4.7 kΩ 到 3V3 或缩短线 |
| `#OLED pullup pin=1`（`pin=2`） | 诊断版：这颗脚量到了「模块自带的上拉」 | 模块有电、共地，且就接在这几颗脚上；接着应出现 `#OLED addr=...` |
| `#OLED pullup none (...) ` | 诊断版：21 个安全 GPIO 开内部下拉后**没有一个**读到高 | 模块没供电 / 没共地 / 线没插上 —— 先量模块焊盘上的 VCC↔GND |
| `#OLED lv scl=1 sda=0` | 诊断版：空闲电平变化时打印 | `sda=0` 说明被拉死；也可拿它做「碰地验线」（见下） |
| `#OLED blind retry=2000ms` | 没读到 ACK：仍在按 200 ms 周期刷新（写总线无害），并每 2 s 轻量 ping 一次 | 屏一旦上线/接线恢复就自动补初始化并打印 `#OLED addr=...`，不用复位 |

**上电顺序不是问题**：探测到屏就整段重发初始化序列（开显示、清屏、全屏刷新）；
没读到 ACK 时初始化序列也照发，并每 2 s 用「只发地址字节」的轻量探测查一次，
所以「先上电 ESP32、后上电 OLED」或运行中插上都能自动点亮。
`#OLED err=nack` 只打一次，不会周期性刷串口。

驱动针对这类「上电不亮」做的七件事（前两条是 2026-11-04 按参考实现修正的）：

- **不再把「读到 ACK」当初始化的前提**：江协 V2.0 的参考软件 I²C 根本不读 ACK，
  在 STM32F103 上照样点亮同一块屏。旧版一 NACK 就跳过整段初始化，等于把能亮的屏挡在门外；
- **ACK 采样读运行时引脚 `s_sda_pin`**：旧版读的是编译期宏 `OLED_SDA_PIN`，
  导致 `OLED_ScanPins()` 扫到的结论全是错的；
- 初始化命令序列与参考实现 `Hardware/OLED.c` 的 `OLED_Init()` **逐条一致**
  （不再额外发 `0xAD 0x8B`：那是 SH1106 的 DC-DC，SSD1306 上 `0xAD` 是内部 IREF 设置，语义不同）；
- 探测 0x3C / 0x3D 两个地址，用有 ACK 的那个；**并自动再试一次 SCL/SDA 互换**（插反也能用）；
- 探测时按 1 µs → 5 µs → 20 µs 逐档降速，成功那档会打在 `spd=` 上；
- 初始化前补 9 个 SCL 时钟做总线自恢复（从机把 SDA 拉死时能救回来），并开内部弱上拉兜底；
- 新增 `OLED_Probe()`（轻量 ping）、`OLED_GetAckCount()` / `OLED_GetNackCount()` /
  `OLED_GetSclPin()` / `OLED_GetSdaPin()`，显示层靠它们做「每 2 s 探一次、通了才补初始化」的重试与诊断。

### 怀疑接错脚时：扫一遍全片引脚

屏在文档引脚上不应答时，可以编一版「扫描固件」：它把 21 个安全 GPIO 两两组合成 (SCL, SDA)
（排除 USB/Flash/PSRAM/JTAG/UART0 和 BOOT 脚 GPIO0），逐个去探测 0x3C/0x3D，
命中的组合直接打在串口上：

```powershell
.\tools\build_ascii.ps1 -Role AP -Disp OLED -BuildDir build_a -Action flash -Port COM3 -ScanAll
```

输出形如：

```
#OLED err=nack scl=1 sda=1 pu=00 scl_pin=1 sda_pin=2
#OLED scan scl=4 sda=5 addr=0x78
#OLED blind retry=2000ms
```

就说明屏其实接在 GPIO4/GPIO5，换回去重编即可：`-SclPin 4 -SdaPin 5`。
（注意：扫描只在 `-DOLED_SCAN_ALL=1` 时才编译进去；2026-11-04 之前 ACK 采样读的是
编译期宏而不是被扫描的引脚，那一轮「506 组合全无应答」的结论无效。）

**2026-11-04 真机重扫（ACK 采样修好之后）**：420 种组合 **0 命中**。
之前的 `sda=0` 假命中已查明是 GPIO0 的问题 —— 它是 BOOT 按键脚、板上并了去抖电容，
被拉低后 10 µs 都回不到高电平，任何地址都会被读成「有 ACK」；扫描已把 GPIO0 排除，
并在每轮开始前检查 SDA 空闲电平，空闲为低就跳过该组合。
结论：屏此刻没有接在这块板子的任何 GPIO 上，或者模块没供电/没共地（`pu=00` 也说明
GPIO1/GPIO2 上量不到模块自带的 4.7 kΩ 上拉），按下面排查表查硬件即可。
扫描版默认关闭（脚本每次显式传 `-DOLED_SCAN_ALL=0`），正式固件里这段代码不参与编译，
既不占启动时间，也不会去碰别的引脚。

### 不用万用表的三步自检（诊断版）

> 2026-11-04 屏已点亮（`#OLED addr=0x78 spd=1us scl=1 sda=2 ack=55`），下面这套留给以后再出现
> 「屏不亮 / 换了线 / 换了板」时用：先 `-ScanAll` 编诊断版，按三步逐根定位，定位完再用正式版覆盖。

诊断版每 2 s 复查一次，插拔线后**不用复位、不用重烧**，串口直接给结果：

1. `#OLED lv scl=1 sda=1` —— 空闲电平。**验线**：把屏那头的 SDA 线头用手碰一下模块的
   GND 焊盘，出现 `#OLED lv scl=1 sda=0` 就说明「这根线 + GPIO2」是通的；
   碰了没反应 ⇒ 线断 / 插错孔 / 这颗脚不是 GPIO2。SCL 同理。
2. `#OLED pullup pin=N` —— 「上拉指纹」：逐个引脚设成**输入 + 内部下拉**再读电平，不发任何波形，
   所以屏不应答也能用。模块板载的 4.7 kΩ 上拉会把所接的脚拉回高电平，
   于是直接报出「屏落在哪几颗脚上」；打 `#OLED pullup none` 就是模块没带电接上。
3. `#OLED sweep 1us … 2000us` —— 速度扫描：把位延时从 1 µs 一路放到 2 ms（250 Hz），
   每档 ping 一次 0x3C/0x3D。全 noack ⇒ 物理上没接通（跟速度无关）；
   只有慢速档 ack ⇒ 线对，但上拉太弱/线太长，补 4.7 kΩ 或缩短杜邦线。

**2026-11-04 实测（屏在 F103 上确认正常，换到 ESP32 后）**：

```
#OLED err=nack scl=1 sda=1 pu=00 scl_pin=1 sda_pin=2
#OLED sweep 1us noack … 2000us noack      ← 11 档全 noack
#OLED lv scl=1 sda=1
#OLED pullup none (no powered module on any scanned pin)
#OLED blind retry=2000ms
```

三条同时成立（`pu=00` / `pullup none` / `2000us noack`）就能下结论：
**屏在电气上没接到这块板子的任何 GPIO**（或模块没电、没共地）。
驱动侧到这里已无可再查，剩下全是接线与供电，按上面三步自检逐根线验。

### 上电流程

1. `#BOOT chip=ESP32-S3 fw=A-1.0.0`
2. 建热点 `ESP32S3-LINK`（WPA2，密码见 `app_config.h`），**信道锁定 6、带宽 HT20**
3. `#APINFO ssid=ESP32S3-LINK bssid=.. ch=6 txp=20.0`
4. 板 B 连上后 OLED 第 3 行出现 `STA1 -45dBm`，第 4 行 `RX` 计数每 50 ms 涨一次

### 命令（每行一条，`\n` 结尾，不区分大小写）

| 命令 | 行为 | 示例输出 |
|---|---|---|
| `P` | 打印热点信息 | `#APINFO ssid=ESP32S3-LINK bssid=24:6f:28:xx:xx:xx ch=6 txp=20.0` + `#OK P` |
| `L` | 采集一次 STA 列表 | 见下 |
| `T=<float>` | 设最大发射功率 2..20 dBm，等 500 ms 后回 | `#TXP set=8.5 raw=34` + `#OK T` |
| `G` | 读回最大发射功率 | `#TXP get=8.5 raw=34` + `#OK G` |
| `BL=<0..100>` | 本板无背光 | `#ERR not_supported` |
| `H` | 帮助 | `#HELP ...` × 6 + `#OK H` |
| 未知命令 | | `#ERR unknown_cmd <原文>` |
| 参数非法 | | `#ERR bad_arg <原文>` |
| `T=` 超范围 | 保持原值 | `#ERR txpower_range` |
| 行超过 64 字节 | 丢弃整行 | `#ERR line_too_long` |
| `L` 执行期间来新命令 | 丢弃 | `#ERR busy` |

`L` 的输出：

```
#STALIST_BEGIN i=3 t_ms=18423
#STALIST n=1
mac,rssi
24:6f:28:aa:bb:cc,-45
#OK L
```

`n=0` 时同样输出 `#STALIST n=0` 与 `mac,rssi` 表头。

### 发射功率换算

`esp_wifi_set_max_tx_power()` 的单位是 **0.25 dBm**：

| dBm | raw（0.25 dBm 单位） |
|---|---|
| 2.0 | 8 |
| 8.5 | 34 |
| 20.0 | 80 |

范围 2..20 dBm 对应 raw 8..80，超出直接回 `#ERR txpower_range`，不调用驱动。
设完延时 500 ms 才回 `#OK T`，让 beacon 先用新功率发出去，否则紧接着的 `L` 读到的
RSSI 还是旧功率下的值。

### OLED 布局（128×64，8×16 点阵，每行 16 列）

| 行 | 坐标 | 内容示例 | 说明 |
|---|---|---|---|
| 1 | (0, 0) | `[A]AP CH6` | 角色 + 信道 |
| 2 | (0, 16) | `TXP:20.0dBm` | 当前最大发射功率 |
| 3 | (0, 32) | `STA1 -45dBm` | 已连 STA 数 + 第一个 STA 的上行 RSSI；无连接时 `STA0 -` |
| 4 | (0, 48) | `RX1234 T:12s` | UDP 保活包计数 + 运行秒数；2 s 无包时前缀变 `NO` |

- 每行先用 `snprintf` 拼成 **定宽 16 列**（不足补空格、超限截断）再写显存，
  避免上次的长数字残留（`-5` 盖不掉 `-45`）；UDP 计数超过 9999 按规格缩位，
  `12345` 显示成 `RX12k3`（12.3k），保证一行不超过 16 列。
- 数值没变就不重绘该行；只有变化的行才 `OLED_UpdateArea(0, row*16, 128, 16)`。
- 每次真正刷新时同步打印一行串口：
  `[DISP] [A]AP CH6 TXP:20.0dBm STA0 - RX1234 T:12s`
  （与屏幕四行逐字对应；用 `Console_Line` 而不是 `ESP_LOGI`，因为日志前缀
  `I (123)` 会把 `[DISP]` 顶到行中间，且默认日志级别 WARN 会把它吞掉）
- 刷新周期 200 ms（≤5 Hz）是硬上限，`L` 结束后的补刷也只把刷新提前到 200 ms
  边界，不会突破 5 Hz。任务在 **core 0**、优先级 1；Wi-Fi/UDP/命令任务在 core 1。
- `L` 输出数据块期间把 `g_ui_paused` 置位并等显示任务退出渲染临界区，
  所以 `[DISP]` 不会插进 `#STALIST_BEGIN` 和数据行之间；`L` 结束再补刷一次。

### 为什么屏幕只用 ASCII

参考驱动（江协科技 V2.0）是 STM32 + GB2312 汉字字库，字模按 GB2312 双字节索引，
源码本身还是 GBK。移植时保留 8×16 ASCII 字模、删掉汉字表和查询分支，好处是：

- 不用处理 GBK → UTF-8 转换，也不会踩到「GBK 次字节是 `0x5C`（`\`）」的编译坑；
- 一张 8×16 字模 16 字节，95 个字符只占 1.5 KB flash；
- 屏上内容与 `[DISP]` 日志一一对应，不会出现乱码方块。

### 为什么软件 I2C 必须加电平间延时

ESP32-S3 跑 240 MHz，`gpio_set_level()` 之间几乎没有间隔，位翻转很容易超过 SSD1306
的 400 kHz 上限，表现为不亮、花屏或随机丢像素。每次电平变化后调用
`esp_rom_delay_us(OLED_I2C_DELAY_US)`，默认 1 µs（一个 bit 三次延时 ≈ 3 µs ≈ 330 kHz），
偏快/偏慢时改这个宏。

读 ACK 的那一拍另外多等 `OLED_ACK_SETTLE_US`（10 µs）：只开内部弱上拉（约 45 kΩ）时，
长杜邦线/面包板的分布电容会让 SDA 上升沿变慢，等太短会把 NACK 误判成 ACK
（实机上就踩过：GPIO0 上并了 BOOT 键去抖电容，2 µs 时任何地址都「有 ACK」）。

### UDP 保活接收

- 绑定 `0.0.0.0:3333`，`recvfrom` 循环读走并丢弃，只累加计数（不逐包打印）。
  不读走会让 lwIP 接收缓冲溢出，进而影响 Wi-Fi。
- 连续 2 s 没有包只把屏幕前缀改成 `NO`，**不会断开任何 STA**。
- 板 A 只收不发；板 B 每 50 ms 发一个 32 字节包。

## 板 A 自测记录

在本机（串口 COM3，ESP32-S3 模组，flash 按 2 MB 布局烧写）上实际跑过的项目：

| 项 | 操作 | 实测输出 |
|---|---|---|
| 上电 | 复位 | `#BOOT chip=ESP32-S3 fw=A-1.0.0` → `#APINFO ssid=ESP32S3-LINK bssid=44:bd:8d:6b:1b:19 ch=6 txp=20.0` |
| 屏自检 | 复位 | **接屏实测（2026-11-04）** `#OLED addr=0x78 spd=1us scl=1 sda=2 ack=55`，屏点亮；屏未接时则打 `#OLED err=nack scl=1 sda=1 pu=00 scl_pin=1 sda_pin=2` + `#OLED blind retry=2000ms`（只打一次），诊断版还会打 `#OLED sweep 1us…2000us noack`、`#OLED lv scl=1 sda=1`、`#OLED pullup none`，并扫 420 种引脚组合 |
| `P` | `p` | `#APINFO ... ch=6 txp=8.5` + `#OK P` |
| `T=` | `t=8.75` / `t=8.5` / `t=2` | `#TXP set=8.8 raw=35` / `#TXP set=8.5 raw=34` / `#TXP set=2.0 raw=8` + `#OK T`（500 ms 后） |
| `G` | `g` | `#TXP get=8.5 raw=34` + `#OK G` |
| `L` | `l` | `#STALIST_BEGIN i=4 t_ms=131720` / `#STALIST n=0` / `mac,rssi` / `#OK L`，期间没有 `[DISP]` 插入 |
| `BL=` | `bl=50` | `#ERR not_supported` |
| 错误路径 | `t=abc` / `t=` / `z` / 100 字符行 | `#ERR bad_arg abc` / `#ERR bad_arg` / `#ERR unknown_cmd Z` / `#ERR line_too_long` |
| `H` | `h` | `#HELP` × 6 + `#OK H` |
| `[DISP]` | 随时 | `[DISP] [A]AP CH6 TXP:8.5dBm STA0 - NO0 T:157s`，与屏幕四行逐字一致 |
| 构建 | 三种配置 | `AP/OLED`（正式）、`AP/OLED -ScanAll`（诊断）、`STA/LCD`（板 B 占位）均编译链接通过，无 warning |

还没在真机上跑的两项需要一个客户端（手机或板 B）连上热点：
`STA1 -45dBm` 那行、`RX` 计数增长与停发 2 s 后的 `NO`。

## 目录结构

```
CMakeLists.txt              顶层：APP_ROLE / DISP_KIND 缓存变量与校验
sdkconfig.defaults          公共默认配置（纯 ASCII 注释）
tools/build_ascii.ps1       中文路径下的构建/烧写中转脚本（见「构建」）
main/
  app_config.h              所有可调参数
  main.c                    初始化与任务创建（无任何 OLED_ 前缀）
  console.c/.h              UART0 115200 + Console_Line（不带日志前缀）
  wifi_link.c/.h            SoftAP、信道/带宽锁定、STA 事件
  ap_measure.c/.h           STA 列表、发射功率设/读
  uart_cmd.c/.h             命令行解析（P/L/T=/G/BL=/H）
  udp_sink.c/.h             UDP 3333 接收与 2 s 超时判定
  display.h                 屏无关状态 disp_state_t 与 Disp_* 接口
  display.c                 按 DISP_KIND 分派、g_ui_paused 互斥、显示任务
  display_oled.c            OLED 后端（定宽行、按行局部刷新、[DISP] 同步）
  display_lcd.c             LCD 后端占位（TODO：板 B）
  display_backend.h         两个后端共用的私有接口
components/oled/             SSD1306 软件 I2C 驱动（OLED.c/.h + 8×16 字模）
```
