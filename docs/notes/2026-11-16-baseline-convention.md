# Agent Note: 基准取「最近一次 case 0」而非首尾平均

Status: implemented

## Problem

穿透损耗是两个中位数之差（`loss = base_median − median`），基准怎么取直接决定每个测点的数值。
固件（`board b/main/measure.c`）与上位机（`master/pc/analyze.py`）目前取法不同，
同一批数据会算出不同的 LOSS，容易被当成 bug 排查。

## Decision

固件在 `case_id == APP_CASE_BASELINE` 完成时覆盖 `s_base_dbm`，后续测点一律用「最近一次基准」做差。

上位机 `analyze.py` 的 `base_statistics()` 取 `first_base` 与 `last_base` 两次基准的中位数平均。

## Alternatives considered

**为什么固件不也用首尾平均？**
固件要在采集过程中实时显示 LOSS，此时尾部基准尚未产生，只能用已经测到的最近一次。
这是运行时约束，不是疏忽。

**为什么脚本不改用最近一次？**
脚本做离线分析，手里同时有首尾两次基准，平均可以降低基准本身的不确定度，
还能顺带用首尾之差评估环境漂移（`DRIFT_LIMIT_DB = 3.0` 的判据就依赖这两次）。

**为什么不统一成一种？**
两种取法服务两个场景（实时显示 / 离线分析），统一会牺牲其中一个。
把它们统一成「最近一次」会让脚本失去漂移检测能力；统一成「首尾平均」会让固件无法实时显示。

## Consequences

同一批数据两处算出的 LOSS 相差 `0.5 × (last − first)` 量级。
这一点需要在 README 明写，否则读者会把它当成缺陷去排查。

若将来固件需要支持「采集中回看」，可以考虑在采集结束后用首尾平均重算一遍并单独显示，但不要覆盖实时的那个读数。
