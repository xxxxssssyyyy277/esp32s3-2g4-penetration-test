# PC 上位机：板 B（STA）遮挡穿透损耗采集与分析

两块 ESP32 做 2.4 GHz 遮挡穿透损耗实验：

* **板 A（AP）**：开热点，固定信道，用来提供稳定的下行信号；
* **板 B（STA + LCD）**：连上板 A，按固件里的测点表逐轮扫描 RSSI，从串口吐出数据；
* **本目录（PC）**：读串口、落原始数据、算损耗、出图和表。

```
pc/
├── serial_proto.py     # 串口协议解析（CL / C= / N= / I= / P / T= / G / L）
├── capture.py          # 需求一：采集 data/raw.csv
├── analyze.py          # 需求二：算损耗 → data/*.csv + figures/*.png
├── demo_offline.py     # 无硬件调试入口：假板子跑完采集+分析（调试用，不参与交付流程）
├── requirements.txt
├── data/
│   ├── materials.csv   # 手工材质表模板（必须按实物改厚度/材质）
│   ├── raw.csv         # 采集产出（运行后才有）
│   ├── summary.csv     # 分析产出
│   └── compare_theory.csv
├── figures/            # 分析产出的 300 dpi 图
└── tests/              # 无需硬件的离线自测
```

---

## 1. 解释器与依赖

本机用的是 **32 位 Python 3.11.2**：

```
D:\Python\Python311-32\python.exe
```

> `D:\Python\python-3.11.2.exe` 是安装包本身，不是解释器；系统 PATH 里的 `python`
> 是乐鑫 IDF 自带的那份，别拿它跑本目录的脚本。

依赖已经装在这个解释器里了（pyserial 3.5 / numpy 1.26.4 / pandas 2.0.3 /
matplotlib 3.7.5）。换机器或在别处重装：

```powershell
cd D:\Homework-reated\project\master\pc
& 'D:\Python\Python311-32\python.exe' -m pip install -i https://pypi.tuna.tsinghua.edu.cn/simple -r requirements.txt
```

32 位 Windows 的最后一批轮子就是上面这三个版本，再新的只有源码包（pip 会去编译
meson/C++ 然后报 `Microsoft Visual C++ 14.0 or greater is required`），所以
`requirements.txt` 里锁了上限并加了 `--only-binary=:all:`；换 64 位 Python 3.12+
可以把上限放宽成 `pandas>=2.2` 之类。

只跑 `analyze.py --help` 或串口解析不需要第三方库（全部延迟导入）；
真正采集需要 `pyserial`，真正分析需要 `pandas` + `numpy` + `matplotlib`。

## 2. 一条从头跑通的命令序列

```powershell
cd D:\Homework-reated\project\master\pc
$py = 'D:\Python\Python311-32\python.exe'           # 本机 32 位 Python 3.11

# ① 先确认连线和固件测点表（只采 2 轮基准，probe.csv 仅烟雾测试，不要拿去分析）
& $py capture.py --port COM5 --cases 0 --n 10 --settle 0 --out data\probe.csv

# ② 填 data\materials.csv：material / thickness_mm / category 必须对应实物
notepad data\materials.csv

# ③ 正式采集：首尾自动各插一次基准（C0），共 12 轮
& $py capture.py --port COM5 --cases 0-10 --n 50 --interval 100 --settle 5 --out data\raw.csv

# ④ 分析 + 出图（日期写进图标题，便于报告复现）
& $py analyze.py --date 2026-11-06
```

`COM5` 换成板 B 的实际串口（设备管理器 → 端口）。板 B 必须已经连上板 A 的热点，
否则 `#APINFO` 没有 bssid、数据行 rssi 全空，脚本会明确警告而不是默默出空图。

`--settle 5` 是每轮开始前留给人插/换遮挡物的时间；`--n 50`、`--interval 100`
约 5 s 一轮，12 轮 ≈ 1 分钟（不含换料时间）。

### 2.1 测点表（与固件 `APP_CASE_LIST` 一一对应）

| 编号 | 标签 | 遮挡物 | 类别 | 备注 |
| --- | --- | --- | --- | --- |
| 0 | `NONE` | 无遮挡 | reference | 基准，首尾各测一次查漂移 |
| 1 | `PAPER` | 硬纸板 1 层（≈3 mm） | dielectric | |
| 2 | `BOOK1` | 1 本书（≈25 mm） | water | 与 3 号构成厚度序列 |
| 3 | `BOOK5` | 5 本书（≈125 mm） | water | |
| 4 | `WATER` | 水墙 6 瓶并排（≈100 mm） | water | 单瓶太小 |
| 5 | `FOIL` | 铝箔 2 层无缝隙 | conductor | |
| 6 | `FOILGAP` | 铝箔 2 层中间 5 cm 缝 | conductor | 与 5 号只差那条缝 |
| 7 | `BODY` | 人体站链路中点 | water | 远场人体 |
| 8 | `HAND_A1` | **手直接捂住板 A 的天线（被试 1）** | water | 近场吸收 + 失谐，RSSI 变化最明显 |
| 9 | `HAND_A2` | 同上，换被试 2 | water | 手掌大小/干湿/握力不同，损耗不同 |
| 10 | `HAND_A3` | 同上，换被试 3 | water | 用于量化「不同人捂」的人际差异 |

8–10 号是近场工况：手掌几乎贴住板 A 的天线，机制是**近场吸收 + 天线失谐**，
不是平板遮挡的平面波穿透，所以损耗常比 7 号「远场人体」大得多，会超出 `water`
的经验量级（3–15 dB）而被判「超出」——这正好是报告里「偏差最大项及原因」的素材。
厚度记为 0，厚度拟合会自动跳过它（`analyze.py` 只拟合 `thickness_mm > 0` 的点）。

**为什么拆成 3 个测点而不是 1 个**：手掌对天线的加载是一个「贴合程度敏感」的近场
问题，被试的手掌面积、皮肤干湿（出汗/洗净）、握持力度、是否戴手表戒指，都会改变
等效加载体积，实测损耗几个人能差 3–8 dB。一个编号只能给出一个人的值，把 3 个人
分成 3 个编号，`loss_by_material.png` 里就能直接看出组间差异，报告可写「人际极差」。
固件测点表是编译期固定的，3 个是预先留好的槽位；如果只找到 2 个人，采集时用
`--cases 0-9`（不含 C10）即可——`materials.csv` 里多出来的 C10 行只会触发一条
`[WARN] 材质表里的 [10] 本次没有采集，已忽略`，不影响出图和判定（实测过）。

要加第 4 个人：`board b/main/app_config.h` 里 `X(10, HAND_A3)` 行末补 `\`，再加
`X(11, HAND_A4)`，`APP_CASE_COUNT` 改 12；`materials.csv` 添一行 `11,HAND_A4,...`；
采集命令改 `--cases 0-11`。标签 `HAND_A4` 仍 ≤7 字符，屏幕和 CSV 都不用改。

**多人测量要统一的条件**（否则差异里混进了操作误差）：

1. 手掌**贴住**板 A 天线区（不是悬空 1 cm），位置每次相同，可在板子边缘贴小标记；
2. 坐着不动，不要用手臂压住桌面或挡住板 B 侧链路；
3. 摘掉手表、戒指等金属饰物，手心不要出汗（出汗会显著加大损耗）；
4. 每人只测自己那一个编号，换人时就把 `--settle 5` 的时间用来交接；
5. `materials.csv` 的 `material` 列写清「被试几」，`note` 列记手掌大致长宽、干湿、
   是否戴金属物，报告里引用这些人因信息。

## 3. capture.py 参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--port` | 必填 | 板 B 串口，如 `COM5` |
| `--baud` | 115200 | 与固件 `UART_BAUD` 一致 |
| `--cases` | `0,1,2,3,4,5,6,7,8,9,10` | 要测的 case_id，支持 `0,1,2` 与 `0-10` 混写 |
| `--n` | 50 | 每测点采样数（固件范围 10–500） |
| `--interval` | 固件默认 100 ms | 采样间隔（20–1000 ms） |
| `--settle` | 5 | 每轮开始前的换料倒计时（秒） |
| `--base` | 0 | 基准测点（无遮挡），首尾各采一次 |
| `--out` | `data/raw.csv` | 原始数据输出 |
| `--timeout` | 120 | 单轮超时（秒） |
| `--retries` | 3 | 单轮最多尝试次数，仍失败则保留已采数据并退出码 3 |

采集流程：`CL` 拿测点表 → 校验 `--cases` → `N=`/`I=` → `P` → 按 `0,1..10,0` 逐轮
`C=<id>`；每轮读 `#BEGIN` → 数据行 → `#SUMMARY` → `#END`，并用 `#SUMMARY` 的
中位数/有效样本数交叉校验。屏幕日志 `[DISP]` 全部丢弃，`#` 元数据行只计入统计。

退出码：`0` 成功；`2` 参数错误；`3` 采集中断（数据已落盘）；`130` Ctrl+C。

## 4. analyze.py 参数

| 参数 | 默认 | 说明 |
| --- | --- | --- |
| `--input` | `data/raw.csv` | 原始数据 |
| `--materials` | `data/materials.csv` | 手工材质表 |
| `--outdir` | `data` | `summary.csv`、`compare_theory.csv` |
| `--figdir` | `figures` | 三张图 |
| `--date` | 今天 | 图标题里的日期 |
| `--lang` | `zh` | 图内语言；找不到中文字体时自动退回 `en` |
| `--no-plots` | 关 | 只算表不出图（调试用） |

处理规则（与提示词 0.2 一致）：

1. 按 `round_role` 区分 `first_base` / `case` / `last_base`；缺基准直接报错；
2. 离群剔除：每个测点内 `|rssi − 中位数| > 3×MAD` 的样本剔除（空 rssi 不算离群，
   单独计为无效样本）；
3. `σ_base` = 首尾两次标准差的均方根，基准中位数 = 首尾中位数的平均；
4. `loss_db = median(base) − median(case)`，`σ_loss = sqrt(σ_base² + σ_case²)`；
5. **首尾基准差 > 3 dB → 退出码 3，不写表也不出图**，错误信息给出差值和可能原因；
6. 判定：实测与参考量级（dielectric 0.5–5、water 3–15、conductor 20–40、
   mesh 15–30 dB，经验量级而非严格理论）相差 ±3 dB 以内记「符合」，否则「超出/低于」。

## 5. 输出

| 文件 | 内容 |
| --- | --- |
| `data/summary.csv` | `case_id,label,material,thickness_mm,category,n,median,stdev,loss_db,sigma_loss` |
| `data/compare_theory.csv` | `case_id,label,category,loss_measured,loss_ref_low,loss_ref_high,verdict` |
| `figures/loss_by_material.png` | 水平柱状图（宽 8 英寸、高度随测点数自适应；10 个测点 → 10 条柱）+ 误差棒 + 按 category 着色 + 柱旁标厚度 |
| `figures/baseline_drift.png` | 6×4 首尾基准箱线 + 散点 + 中位数线 + 首尾差值 |
| `figures/loss_by_thickness.png` | 6×4 损耗–厚度散点 + 线性拟合 + dB/mm 斜率 |

厚度图只在某一类存在 ≥2 个不同厚度时生成：优先 `dielectric`；模板里的厚度序列
（BOOK1 25 mm / BOOK5 125 mm）属于 `water`，所以 `water` 达标时也会画，标题里注明
实际用的是哪一类。`thickness_mm = 0` 的测点（C7 `BODY`、C8–C10 `HAND_A1..3`，
厚度不适用）不参与拟合。全部 PNG 为 300 dpi、白色不透明背景。

CSV 用 `utf-8-sig` 写出，Excel 双击即可正常显示中文。

## 6. 离线自测（无需硬件）

```powershell
cd D:\Homework-reated\project\master
& 'D:\Python\Python311-32\python.exe' -m unittest discover -s pc/tests -v
```

当前结果：**Ran 12 tests, OK**（`test_offline.py` 8 个、`test_analyze.py` 4 个；
没装 pandas 时 `test_analyze` 整类跳过，其余仍然全绿）。

* `tests/fake_board.py` 按固件格式应答，并在数据里掺 `[DISP]` 行、空 rssi、半路掉线；
* `tests/test_offline.py` 覆盖命令顺序、12 轮结构、空样本保留、掉线重试与报错；
* `tests/test_analyze.py` 端到端验证损耗数值、判定、3 个手捂测点的人际极差、漂移拒绘图
  （未装 pandas 时整类跳过）。

## 7. 在 VS Code 里调试

**一次配置**：用 VS Code 打开 `master` 目录（不是 `pc`），`Ctrl+Shift+P` →
`Python: Select Interpreter` → `D:\Python\Python311-32\python.exe`。
仓库里已带 `.vscode/launch.json` 和 `.vscode/settings.json`（解释器路径、`pc/` 的
import 路径、测试面板参数都配好了）。

`Ctrl+Shift+D` 打开「运行和调试」，右上角下拉框里选：

| 配置 | 干什么 | 需要硬件 |
| --- | --- | --- |
| ① 假板子跑全流程 | `demo_offline.py`：假板子采 12 轮 → 直接分析出图，`pc/demo/` | 否 |
| ①b 假板子 + C6 掉线 | 演示「重试 3 次 → 退出码 3 → 部分数据落盘」 | 否 |
| ② 采集 capture.py（真串口） | 12 轮正式采集，会弹框问 COM 号 | 是 |
| ②b 采集（只测 C1） | `--cases 1 --n 10 --settle 0`，几秒跑完，适合下断点 | 是 |
| ②c 采集（只测 C8 手捂板 A 被试 1） | `--cases 8 --n 20 --settle 0`，验证新测点是否生效 | 是 |
| ②d 采集（三个被试一起测） | `--cases 0,8-10 --n 20 --settle 5`，5 轮（0→8→9→10→0），专看人际差异 | 是 |
| ③ 分析 analyze.py | 读现有 `data/raw.csv`（带 `--no-plots`，跑得快） | 否 |
| ④ 离线自测 | `unittest discover`，可以在测试里下断点 | 否 |
| ⑤ 当前文件 | 调 `demo_offline.py` 或临时脚本时用 | 视文件 |

下断点的位置建议：

| 想看什么 | 文件 / 函数 |
| --- | --- |
| 每一行串口数据怎么解析 | `serial_proto.py` → `classify_line` / `parse_data_line` |
| 单轮采集的状态（收到几行、`#BEGIN` 对不对） | `capture.py` → `measure_once` |
| 重试和丢弃逻辑 | `capture.py` → `measure_with_retry` 的 `except IncompleteRound` 分支 |
| 首尾基准 / 漂移拒绘图 | `analyze.py` → `base_statistics` / `ensure_drift_ok` |
| 损耗数值算错 | `analyze.py` → `remove_outliers` / `compute_summary` |
| 图不对 | `analyze.py` → `plot_loss_by_material` / `plot_loss_by_thickness` |

调试技巧：

* **条件断点**（右键断点 → 编辑断点）：`len(rows) == 25`、`payload["case_id"] == 5`、
  `case_id == 0 and role == "last_base"`；
* **日志断点**（Logpoint，不暂停）：`C{case_id} seq={payload['seq']} rssi={payload['rssi_dbm']}`，
  适合一口气看 450 行数据；
* **调试控制台**里可以直接算：`stats.data_rows`、`stats.report()`、`rows[-1]`、
  `frame.groupby("case_id")["rssi_dbm"].median()`、`cleaned.head()`；
* `justMyCode` 已设为 `false`，可以 `F11` 单步进 pandas/matplotlib 内部看调用栈。

假板子还能按需注入故障（改 `demo_offline.py` 里的 `FakeBoard(...)`，或复制
`tests/test_offline.py` 的写法）：`drop_cases=[6]`（半路掉线）、`error_cases=[5]`
（固件 `#ERR`）、`empty_every=3`（RSSI 大量为空）、`fail_after=7`（掉线前先吐 7 行）、
`base_rssi=-70`（链路很差的场景）。

> 调试**真串口**时注意：断点一停，板子还在发数据，串口缓冲区会积压；恢复运行后
> 可能读到几行"旧数据"（表现为 `#SUMMARY` 与上位机统计不一致的 `[WARN]`）。
> 想在硬件上单步，就用「②b 只测 C1」这种小参数，或者直接对着 FakeBoard 的原始行
> 字符串调 `serial_proto.classify_line()`。

## 8. 常见问题

| 现象 | 原因 / 处理 |
| --- | --- |
| `#APINFO` 没有 bssid | 板 B 没连上板 A 热点，先确认 ssid/密码与信道 |
| 数据行 rssi 大量为空 | 链路太差或板 A 没发包；缩短距离重测 |
| 首尾基准差 > 3 dB | 有人走动/碰了板子/换了 AP；恢复现场整批重测 |
| 图上中文变方块 | 装 Microsoft YaHei / SimHei，或 `& $py analyze.py --lang en` |
| `--cases` 报测点不存在 | 固件 `measure.c` 的用例表与本机不同，先 `CL` 看实际表 |
| `C=8..10` / `--cases 8-10` 报 `#ERR bad_case` | 板 B 还是旧固件（`APP_CASE_COUNT` 只有 8 项）；把 `board b` 重新 `idf.py build flash` 后再试 |
| 损耗异常小 | 遮挡物小于第一菲涅尔区（1 m 时半径 17.7 cm），需 ≥35×35 cm 且垂直链路 |
| pip 报 `Microsoft Visual C++ 14.0 or greater is required` | 32 位 Python 下装到了没有 win32 轮子的新版本；用 `-r requirements.txt`（已锁上限并带 `--only-binary=:all:`） |
| pip 报 `UnicodeDecodeError: 'gbk' codec can't decode` | requirements.txt 里不要写中文注释（pip 22 用 GBK 读文件） |
| `python: 无法将…识别为 cmdlet` / 版本不对 | PATH 里的 `python` 是乐鑫 IDF 的；按本机路径 `& 'D:\Python\Python311-32\python.exe'` 调用 |
