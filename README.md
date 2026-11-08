# ESP32-S3 2.4 GHz 遮挡穿透损耗测量平台

《工程电磁场》课程报告的配套代码。用两块 ESP32-S3 搭建一套可复现的 2.4 GHz 链路测量平台，
按「遮挡物插入链路中点」的方式测量接收电平的相对变化，即穿透损耗。

本仓库是报告第 3、4 章所述平台的完整实现：两块板的固件与上位机分析脚本。

---

## 仓库结构

| 目录 | 内容 | 语言 / 环境 |
|---|---|---|
| `board a/` | **板 A 固件**：SoftAP（信道 6、20 MHz）+ 0.96″ OLED 状态显示 + `L`/`T=`/`G` 命令 + UDP 保活接收 | C / ESP-IDF v5.1.2 |
| `board b/` | **板 B 固件**：STA + 1.54″ 240×240 LCD + 按测点批量采 RSSI → CSV + UDP 保活发送 | C / ESP-IDF v5.1.2 |
| `master/` | **上位机**：串口采集、MAD 去离群、穿透损耗计算、出图出表，含离线自测 | Python 3.11 |

两块板是**独立工程**，各自编译烧写，通过 Wi-Fi 交互。

---

## 硬件

| | 板 A | 板 B |
|---|---|---|
| 芯片 | ESP32-S3 | ESP32-S3 |
| 角色 | SoftAP（发射端） | STA（接收端） |
| 屏 | 0.96″ 128×64 OLED（SSD1306，软件 I2C） | 1.54″ 240×240 LCD（ST7789V，4 线软件 SPI） |
| 屏引脚 | SCL=GPIO1、SDA=GPIO2 | SCL=39、SDA=40、RES=41、DC=42、CS=2、BL=1 |
| 测什么 | **上行** RSSI（`esp_wifi_ap_get_sta_list`） | **下行** RSSI（`esp_wifi_sta_get_ap_info`） |
| 控什么 | 发射功率 `T=`（2–20 dBm，0.25 dBm 步进） | 采样参数 `N=` / `I=`、保活开关 `U=` |

两板间距固定 1.00 m，天线等高同向、视距无遮挡，构成基准工况。

> ⚠️ GPIO1/GPIO2 在板 B 上被 LCD 用作 BL/CS，但那是**另一块独立的板**，互不影响。
> 不要用 GPIO37（带 Octal PSRAM 的模组上是 PSRAM 的 DQS/DM）、GPIO38（FSPIWP）、以及 GPIO9–14（SPI2 默认 IOMUX）。

---

## 快速开始

### 1. 构建与烧写

**ESP-IDF v5.1.2 在 zh-CN Windows 上无法在含中文的路径里编译**，两个工程都提供了 ASCII 镜像脚本：

```powershell
# 板 A
cd "board a"
.\tools\build_ascii.ps1 -Role AP  -Disp OLED -BuildDir build_a -Action flash -Port COM_A

# 板 B
cd "board b"
.\tools\build_ascii.ps1 -Role STA -Disp LCD  -BuildDir build_b -Action flash -Port COM_B
```

脚本会 `robocopy /MIR` 到 `C:\Users\Public\esp32_build\esp32s3_link`，再在那里调用 `idf.py`。

### 2. 串口命令

**板 A（SoftAP）**

| 命令 | 作用 |
|---|---|
| `P` | 打印 `#APINFO ssid/bssid/ch/txp` |
| `L` | 读已连接 STA 列表 → `#STALIST_BEGIN` … `mac,rssi` |
| `T=<2..20>` | 设发射功率（dBm），回 `#TXP set=<dBm> raw=<0.25dBm>` |
| `G` | 读发射功率 |
| `BL=<n>` | 本板无背光 → `#ERR not_supported` |

**板 B（STA）**

| 命令 | 作用 |
|---|---|
| `C=<0..10>` | 设测点并采集一轮，输出 `<case_id,seq,t_ms,rssi_dbm>` |
| `CL` | 打印 11 个测点 |
| `N=<10..500>` | 每轮采样数（默认 50） |
| `I=<20..1000>` | 采样间隔 ms（默认 100） |
| `U=<0\|1>` | 开始 / 停止 UDP 保活（板 A 侧 RSSI 依赖它更新） |
| `BL=` / `BLDIM=` | 背光亮度 / 采集时亮度 |

### 3. 上位机

```powershell
cd master\pc
$py = 'D:\Python\Python311-32\python.exe'

# ① 烟雾测试
& $py capture.py --port COM5 --cases 0 --n 10 --settle 0 --out data\probe.csv

# ② 按实物改材质表（厚度必须真实）
notepad data\materials.csv

# ③ 正式采集：首尾自动各插一次基准，共 12 轮
& $py capture.py --port COM5 --cases 0-10 --n 50 --interval 100 --settle 5 --out data\raw.csv

# ④ 分析出表与图
& $py analyze.py --date 2026-11-08
```

无硬件时可以先跑离线自测与假板子演示：

```powershell
cd master
& $py -m unittest discover -s pc/tests -v      # 12 个测试
cd pc
& $py demo_offline.py                          # 假板子跑全流程 → pc/demo/
```

---

## 测点表

| 编号 | 标签 | 遮挡物 | 类别 |
|---|---|---|---|
| 0 | `NONE` | 无遮挡（基准，首尾各测一次） | reference |
| 1 | `PAPER` | 硬纸板 1 层 ≈3 mm | dielectric |
| 2 | `BOOK1` | 1 本书 ≈25 mm | water |
| 3 | `BOOK5` | 5 本书 ≈125 mm | water |
| 4 | `WATER` | 水墙（6 瓶并排）≈100 mm | water |
| 5 | `FOIL` | 铝箔 2 层无缝隙 | conductor |
| 6 | `FOILGAP` | 铝箔 2 层中间留 5 cm 缝 | conductor |
| 7 | `BODY` | 人体站在链路中点 | water |
| 8–10 | `HAND_A1..3` | 手掌贴住板 A 天线（三名被试） | water |

---

## 实测结论（详见报告第 4 章）

本平台记录了三类与直觉不符的现象，并给出机理解释：

1. **干燥纸介质无可分辨响应**：25 mm / 125 mm 纸堆的穿透损耗估计仅 0.1–1.5 dB，低于 RSSI 的 1 dB 分辨率，
   且书本尺寸（15×21 cm）小于 1 m 间距下的第一菲涅耳区（半径 17.7 cm，需 ≥35×35 cm 遮挡面），
   电磁波从边缘绕射通过。
2. **手掌遮挡天线产生强烈响应**：天线工作在近场（λ/2π ≈ 1.91 cm），手掌即成为天线的一部分，
   以**阻抗失谐**为主因，衰减甚至超过远处的整块金属遮挡。
3. **金属盒全罩导致链路中断而非读数下降**：RSSI 是**解帧量而非场强量**，完全屏蔽时收不到帧、
   没有样本（`valid=0`），只能报告“链路中断”。

由此得出本方法的**有效区间约 1～30 dB**：下限由 RSSI 分辨率与多径抖动决定，上限由链路维持能力
（信标丢失与重连）决定，远早于 −98.4 dBm 的接收灵敏度。

---

## 说明

- 固件中的热点口令 `12345678` 是本地实验用的示例值，不含任何真实凭据。
- `master/pc/data/materials.csv` 是材质表**模板**，厚度需按实物修改后重跑分析。
- 采集/分析产出（`raw.csv`、`summary.csv`、`figures/` 等）已由 `.gitignore` 排除，可由脚本再生。
