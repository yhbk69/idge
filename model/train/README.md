# model/train —— 训练与转换侧的源材料

**程序不读这个目录**。运行时只认 `model/library/<id>/`（`ModelRegistry` 扫那里），
所以这里的 `.pt` / `.yaml` / 校准清单改来改去都不会影响现场行为；反过来，
光把 `.pt` 放这儿也不会让模型生效——必须转成 `.rknn` 收编进 `library/`。

```
train/
├── README.md                    # 本文件
├── calib/                       # INT8 量化校准清单（python/convert.py 的 DATASET 环境变量/默认清单）
│   ├── dataset.txt              # 单行占位（assets/test/bus.jpg，仅供跑通流程）
│   ├── dataset_test.txt         # 两行占位（bus.jpg + test.jpg，同上）
│   └── dataset_onnx_generic.txt # 两行占位（ladder/fire 转换用；域内校准集请另行覆盖）
├── cigarette/
│   └── yolo11n-cigarette.pt     # 吸烟检测权重（Ultralytics YOLO11n，nc=1，names={0:cigarette}，imgsz=640）
└── ppe/
    └── ppe.yaml                 # PPE 数据集配置（5 类：un_helmet/helmet/person/vest/workWear）
```

## 为什么这些不进 runtime

| 文件 | 谁读它 |
| --- | --- |
| `*.pt` | 只有 PC 上的 ultralytics；板上 C++ 只加载 `.rknn` |
| `*.yaml` | 只有训练时的数据集定义；类别表的运行时事实源是 `library/<id>/labels.txt` |
| `calib/*.txt` | 只有 `python/convert.py` 量化那一步 |

`*.pt` / `*.onnx` / `*.rknn` 都在 `.gitignore` 里（二进制不进 git，分发靠整库 zip 或设置页「导入模型」）。

## 转换链路（x86 PC 或板端均可）

`rknn-toolkit2`（`from rknn.api import RKNN`）旧版只发 x86_64 Linux 的 wheel，
**2.3.0 起提供 aarch64 wheel**：2026-11-10 在本板（RK3588）实测 load_onnx/build/export 全通，
ladder/fire 两个 rknn 就是板端转的（`python/convert.py` 头部旧注释已改）。
已导出的 onnx 源权重收在 `model/onnx/`（规格、sha256 与转换命令见其 README）：

```
.pt ──(ultralytics: yolo export)──> .onnx ──(rknn-toolkit2: convert.py)──> .rknn
                                （收 model/onnx/）                    └──> model/library/<id>/model.rknn
```

一步做完（仓库根目录下）：

```bash
python3 python/pt2rknn.sh model/train/cigarette/yolo11n-cigarette.pt yolo11n-cigarette rk3588 i8
```

脚本细节与手工分步命令见 `python/README.md`。产物落进 `library/<id>/` 后，
按 `library/<id>/README.md` 的「启用步骤」把它写进 `config.json` 的通道模型列表。

⚠ 校准清单里的图片路径**相对清单文件所在目录**解析（rknn 行为）：从 `calib/` 回仓库根
要写 `../../../assets/...`。仓库原有两份清单曾因此一直是坏路径，2026-11-10 已修。
