# 吸烟（手持烟支）检测模型

**用途**: 视频实时检测画面中的烟支，配合 `video.models["yolo11n-cigarette"].alarm_class = "cigarette"` 出吸烟报警。

**状态**: **已入库可用**（`model.rknn` 于 2026-10-28 转换到位，4.6 MB，rk3588 / i8）。
CLI 冒烟通过：输入 `[1,640,640,3]` INT8、scale 0.003922（= 1/255，与本项目预处理约定一致），
输出 `[1,5,8400]` —— 5 = 1 个类别 + 4 个框参数，与上表 `nc=1` 对得上。
`ModelRegistry::rescan()` 只登记存在 `model.rknn` 的目录（`model_registry.cpp:143`），
所以**文件名必须是 `model.rknn`**：早期放进来的 `cigarette.rknn` 不会被识别（已于 10-28 改名修正）。
尚未接进 config——本模型目前不参与任何通道的推理，需按下面「启用步骤」挂接。

---

## 模型规格（从权重文件本身读出，非猜测）

| 项 | 值 | 依据 |
| --- | --- | --- |
| 架构 | YOLO11n（`yolo11n.yaml`，含 `C3k2`/`C2PSA`/`Detect`） | `best/data.pkl` 内的模块名与 `scale=0.25` |
| 任务 | detect（单阶段检测，非分割/姿态） | `ultralytics.nn.tasks.DetectionModel` |
| 类别数 `nc` | **1** | pickle 里 `nc = 1` |
| 类别表 `names` | `{0: "cigarette"}` | 同上，行序即 `cls_id` |
| 训练输入 `imgsz` | **640** | pickle 里 `imgsz = 640` |
| 训练来源 | Windows 端 `D:\dataset\cigarette\data.yaml`，run 名 `cigarette_yolo11n`，基座 `yolo11n.pt` | pickle 里的 `data`/`name`/`model` 参数 |

权重源文件：`model/train/cigarette/yolo11n-cigarette.pt`（原名 `cigratte.pt`，拼写已纠正）。

## 转换产物落位

```
yolo11n-cigarette/
├── model.rknn      # 已生成：x86 PC 上 python/pt2rknn.sh 产出（rk3588 / i8）
├── labels.txt      # cigarette（一行一类，行序 = cls_id）
└── meta.json       # 已按上表填好；model.rknn 到位后 rescan 会补算 sha256（内存里）
```

一步转换（在 x86 Linux/WSL，装有 ultralytics + rknn-toolkit2）：

```bash
python3 python/pt2rknn.sh model/train/cigarette/yolo11n-cigarette.pt yolo11n-cigarette
```

## 启用步骤（转换完成后）

1. 把 `model.rknn` 放进本目录（**文件名固定**，或在 PC 上跑完脚本直接生成）；【已完成】
2. `config.json` 里加参数条目，报警类别就是 `cigarette`：
   ```json
   "video": { "models": { "yolo11n-cigarette": { "alarm_class": "cigarette", "note": "吸烟检测" } } }
   ```
   （设置页「模型参数」表里也能改，等效）
3. 在需要它的通道按槽序追加 id：`"video": { "channels": [ { "models": ["yolo11n-ppe","yolo11n-coco","yolo11n-cigarette"] } ] }`；
4. 重启程序（改模型列表当前不做热更新）。

## 注意事项

- **NPU 预算**：又多一个模型就是多一路推理任务。RK3588 只有 3 个 NPU 核，
  单通道 3 个模型已是上限信号（总数 > 6 会打软告警），加这个模型前先想清楚挂几路。
- **小目标精度**：烟支在 1080p 画面里通常只有几十像素，letterbox 到 640×640 后更小。
  INT8 量化对小目标掉点敏感——校准集要用**含烟支的现场帧**，别用默认的 `assets/test` 通用图；
  若量化后检不出，先改 `fp`（不量化）对照，再考虑 n→s。
- **围栏无关**：围栏触发类别仍是 `geofence.alarmClass`（默认 `person`）。想"进入某区域抽烟才报警"
  需要把围栏类别设成 `cigarette`（本模型能产出它即可触发），这属于通道配置层面的组合，不是新代码。
- 与 `yolo11n-behavior`（预留的行为分析多类模型）不冲突：那个目录规划的是
  fall/phone_call/hot_work 等行为类别，烟支单独成一个模型更快落地。
