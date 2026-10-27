# 音频检测模型（预留）

**用途**: 声音事件检测、语音告警

**规划模型**:
- 玻璃破碎检测
- 尖叫检测
- 爆炸声检测
- 语音播报 (TTS)

**对应 plan**: plan/roadmap.md §九 L5 - 音频能力

**状态**: 预留目录，待模型选型

---

## 目录结构（预期）

```
audio/
├── sound_event/          # 声音事件检测
│   ├── model.rknn
│   ├── labels.txt        # 声音类别
│   └── meta.json
└── tts/                  # 语音合成（可选）
    └── ...
```

## 标签示例（预期）

```
glass_breaking
scream
explosion
alarm
```

## 技术约束

- 音频模型通常不在 NPU 上运行，使用 CPU 推理
- 3rdparty 已集成 libsndfile、FFTW，可用于音频预处理
- 声音检测需实时处理音频流，延迟要求高
