# model/train —— 训练与转换侧的源材料

**程序不读这个目录**。运行时只认 `model/library/<id>/`（`ModelRegistry` 扫那里），
所以这里的 `.pt` / `.yaml` / 校准清单改来改去都不会影响现场行为；反过来，
光把 `.pt` 放这儿也不会让模型生效——必须转成 `.rknn` 收编进 `library/`。

```
train/
├── README.md                    # 本文件
├── calib/                       # INT8 量化校准清单（python/convert.py 的 dataset 参数）
│   ├── dataset.txt              # 单行占位（assets/test 的图，仅供跑通流程）
│   └── dataset_test.txt         # 三行占位（同上）
├── cigarette/
│   └── yolo11n-cigarette.pt     # 吸烟检测权重（Ultralytics YOLO11n，nc=1，names={0:cigarette}，imgsz=640）
└── ppe/
    └── ppe.yaml                 # PPE 数据集配置（5 类：un_helmet/helmet/people/vest/workWear）
```

## 为什么这些不进 runtime

| 文件 | 谁读它 |
| --- | --- |
| `*.pt` | 只有 PC 上的 ultralytics；板上 C++ 只加载 `.rknn` |
| `*.yaml` | 只有训练时的数据集定义；类别表的运行时事实源是 `library/<id>/labels.txt` |
| `calib/*.txt` | 只有 `python/convert.py` 量化那一步 |

`*.pt` / `*.onnx` / `*.rknn` 都在 `.gitignore` 里（二进制不进 git，分发靠整库 zip 或设置页「导入模型」）。

## 转换链路（只能在 x86 PC 上做）

`rknn-toolkit2`（`from rknn.api import RKNN`）**只发 x86_64 Linux 的 wheel**，
板子是 aarch64，装不上也跑不了——这不是本项目的环境缺失，是 Rockchip 工具链的硬限制
（见 `python/README.md` 注意事项）。所以：

```
.pt ──(ultralytics: yolo export)──> .onnx ──(rknn-toolkit2: convert.py)──> .rknn
                                                                   └──> model/library/<id>/model.rknn
```

一步做完（PC，仓库根目录下）：

```bash
python3 python/pt2rknn.sh model/train/cigarette/yolo11n-cigarette.pt yolo11n-cigarette rk3588 i8
```

脚本细节与手工分步命令见 `python/README.md`。产物落进 `library/<id>/` 后，
按 `library/<id>/README.md` 的「启用步骤」把它写进 `config.json` 的通道模型列表。
