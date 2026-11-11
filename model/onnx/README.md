# model/onnx — 转换输入权重（运行时不读）

程序只读 `library/<id>/model.rknn`；本目录是**转换链路的输入事实源**：
原始 ONNX 权重收在这里，一个模型一个固定文件名，转出的 rknn 落 `library/`。
与 `train/` 的分工：`train/` 放训练侧材料（数据集 yaml、量化校准清单），
本目录放**已导出待转换**的 onnx；两者都不参与编译、不被运行期加载。

| 文件 | 类别 | 规格 | 转出目标 |
|------|------|------|----------|
| `ladder.onnx` | 1 类：`ladder` | images 1×3×640×640；split 输出 box_p3/p4/p5(64ch DFL) + cls_p3/p4/p5(1ch)；pytorch 2.4.1 / opset 12 | `library/yolo11n-ladder/model.rknn` |
| `fire.onnx` | 2 类：`fire`、`smoke` | 同上，cls 头 2ch | `library/yolo11n-fire/model.rknn` |
| `tools.onnx` | 24 类工器具（扳手/螺丝刀/钳子/锤/卷尺等，全表见 `library/yolo11n-tools/labels.txt`） | 同上，cls 头 24ch | `library/yolo11n-tools/model.rknn` |

来源：2026-11-10 用户提供的训练导出件（ladder = 登高守护触发源，fire = PDF §6.3.2 场景⑤明火/烟雾）；
2026-11-11 补充 tools（工器具盘点，labels.txt 随模型一并由用户提供，行序 = cls_id）。
sha256：

```
bb79875b180a5f5861afbe799fd4e71dd81b3c7aacdc693935187b137a408e02  ladder.onnx
4a46d9ca4c9c8928142fedc485e759d9a45c1591f52080e8de039c2ac244fc04  fire.onnx
9ac255e60a7c1479a1b0f1189cba09a7000013bcc82a134f638d12670e9a8d07  tools.onnx
```

## 转换

```bash
cd python
DATASET=../model/train/calib/dataset_onnx_generic.txt \
    python3 convert.py ../model/onnx/ladder.onnx rk3588 i8 ../model/library/yolo11n-ladder/model.rknn
DATASET=../model/train/calib/dataset_onnx_generic.txt \
    python3 convert.py ../model/onnx/fire.onnx  rk3588 i8 ../model/library/yolo11n-fire/model.rknn
DATASET=../model/train/calib/dataset_onnx_generic.txt \
    python3 convert.py ../model/onnx/tools.onnx rk3588 i8 ../model/library/yolo11n-tools/model.rknn
```

- **板端 aarch64 也能转**：本机 rknn-toolkit2 2.3.0 实测 load/build/export 全通
  （convert.py 头部"只有 x86_64 wheel"的注释针对旧版本，2.3.0 起不成立）。
- 校准清单里的图片路径**相对清单文件所在目录**解析（rknn 行为），不是相对运行目录：
  从 `model/train/calib/` 到仓库根要写 `../../../assets/...`。仓库原有两份清单
  （`dataset.txt` / `dataset_test.txt`）曾因此一直是坏路径，2026-11-10 已修。
- 当前 i8 校准用的是通用占位图（bus/test），**非梯/火域内图**：量化精度未验证，
  现场验收前拿含梯子/明火/烟雾的帧重转一次（`DATASET=` 覆盖清单即可）。
- onnx 输入固定 640×640：登高守护参考方案在 1080p 全图实测 imgsz 640 会漏梯上的人、
  1280 才稳；板端级联统一 640 letterbox，故**梯上人漏检风险存在**，需现场验证，
  必要时重新导出 1280 输入的 onnx 再转（rknn 输入尺寸跟 onnx 走，转换改不了）。
- `.onnx` 与 `.rknn` 一样不进 git（.gitignore 排除），部署包需自行携带本目录与 `library/`。