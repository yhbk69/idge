# OCR 文字识别模型

**用途**: 智能识别页的铭牌文字提取（车牌、设备编号、安全标识）

**状态**: 已部署（2026-10-26）

## 目录结构

```
ppocrv4-ocr/
├── det.rknn    # PP-OCRv4 文字检测（DB），480x480 INT8
├── rec.rknn    # PP-OCRv4 文字识别（SVTR-LCNet），6625 字符字典
└── labels.txt  # ppocr_keys_v1.txt 字符集（行序 = CTC index - 1）
```

执行体（两个 rknn_model_zoo demo 可执行 + 其 RUNPATH 下的 librknnrt.so）是第三方产物，
放在 `3rdparty/ppocrv4/`，构建与 CLI 说明见 `3rdparty/ppocrv4/README.md`。本目录只存模型数据。

## 调用方式

不在 `library/` 扫描范围内（注册表只收 `model.rknn` 单文件目标检测模型）：
`SmartRecognitionWidget` 用 `QProcess` 起 `3rdparty/ppocrv4/bin/` 里的外部可执行，
参数为 `<det.rknn> <rec.rknn> <image>`，解析 stdout 的
`regconize result: <文本>, score=<置信度>` 行。

## 来源

- 模型：`/home/ubuntu/projects/lubancat_ai_manual_code/example/ppocrv4/model/*_rk3588.rknn`
  （已按 target_platform=rk3588 转换，可直接被 2.3.2 运行时加载）
- 字典：`rknn_model_zoo-v2.3.0/examples/PPOCR/PPOCR-Rec/model/ppocr_keys_v1.txt`

## 备注

- 识别字典同时编译进 demo 可执行的 `dict.h`，`labels.txt` 仅作人工核对用。
- `*.rknn` 被根 `.gitignore` 排除，本目录内容随环境提供；缺失时 OCR 卡片自动降级为不可用。
