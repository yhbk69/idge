# 行为分析检测模型（预留）

**用途**: 视频实时行为分析检测

**规划模型**:
- 跌倒检测 (fall detection)
- 打电话检测 (phone call detection)
- 区域入侵检测 (intrusion detection)
- 动火作业检测 (hot work detection)

> 吸烟检测已从本目录拆出，单独走 `yolo11n-cigarette`（权重与 rknn 均已入库，2026-10-28）：
> 单类小目标模型比"多行为合一"更容易先落地，也别让一个模型的类别表绑住其它行为的迭代节奏。

**参考架构**: MoveNet Lightning 姿态估计 + 行为规则引擎

**对应 plan**: plan/roadmap.md §二 P0-3 / §九 L2 - 行为分析

**状态**: 预留目录，待模型选型与训练

---

## 目录结构（预期）

```
yolo11n-behavior/
├── model.rknn      # YOLO11n INT8 行为分析模型
├── labels.txt      # 行为类别标签
└── meta.json       # 元数据
```

## 标签示例（预期）

```
fall
phone_call
intrusion
hot_work
```
