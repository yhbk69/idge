# 明火/烟雾检测模型（预留）

**用途**: 视频实时明火与烟雾检测

**规划模型**:
- 明火检测 (fire detection)
- 烟雾检测 (smoke detection)

**对应 plan**: plan/roadmap.md §二 P0-3 / §九 - 增强AI能力

**状态**: 预留目录，待模型选型与训练

---

## 目录结构（预期）

```
yolo11n-fire/
├── model.rknn      # YOLO11n INT8 明火/烟雾检测模型
├── labels.txt      # 类别标签
└── meta.json       # 元数据
```

## 标签示例（预期）

```
fire
smoke
```

## 备注

当前系统已通过 ModelRegistry 自动探测 id 含 "fire" 的模型并加载。
放入此目录后，重启即可生效，无需修改代码。
