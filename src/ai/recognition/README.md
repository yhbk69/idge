# src/ai/recognition

> 所属域：**ai 推理与模型应用域**（目录重组 S4c 迁入）

## 功能概述

人脸识别（SCRFD 检测 + ArcFace 风格 512 维特征提取）的两套实现：

- **`InProcessFaceRecognizer`（生产链路）**：进程内常驻两个 rknn 上下文（检测 ~10MB + 识别 ~85MB），直接调 RKNN API 完成"检测→五点仿射对齐→特征提取→L2 归一化"全链，2026-09 从外部 exe 链路内化而来，`RollCallService` 使用本实现；
- **`FaceRecognitionWrapper`（对照/工具链路）**：薄包装，经 fork+execv 拉起 `model/face/face_recognition` 可执行程序、以临时 JSON 文件取回结果。应用内已无生产调用方，保留供 CLI 手工验证与 `test_rollcall` 的移植前后对照回归。

两者输出同一套数据结构（`DetectedFace`/`FaceDetectionResult`，定义在 `face_recognizer.h`），消费方无感切换。

## 文件清单

| 文件 | 职责 |
|---|---|
| `face_recognizer.h` | 数据结构（`DetectedFace`/`FaceDetectionResult`）与 `FaceRecognitionWrapper` 接口声明 |
| `face_recognizer.cpp` | 外部 exe 调起（fork+execv，不经 shell）、nlohmann JSON 解析（bbox/landmarks 双格式兼容、非法框过滤）、点积=余弦的相似度计算 |
| `in_process_face_recognizer.h/.cpp` | 进程内识别器：`init(det.rknn, rec.rknn, core_mask)` 常驻加载、`detectAndExtract(path)` 单图全链推理（推理逻辑自 `tools/face_recognition/main.cpp` 逐段移植） |

## 核心类与数据流

### InProcessFaceRecognizer（生产链路）

```
init(det_rknn, rec_rknn, RKNN_NPU_CORE_AUTO)     // 一次性加载两个上下文，失败全回收
detectAndExtract(image_path)                      // 同步、非线程安全（rknn 上下文）
  → cv::imread 原图
  → detectFaces: 640×640 letterbox(左上对齐补0) → (x-127.5)/128 → rknn_run
      → SCRFD 9 输出解析(score 0/1/2, bbox 3/4/5, kps 6/7/8；stride 8/16/32，每格2 anchor)
      → 分数过滤 → IoU NMS → 仿射逆变换回原图坐标
  → 逐脸 extractFeature: Umeyama 五点相似变换 → warpAffine 112×112
      → (x-127.5)/127.5 → rknn_run → 512 维特征 + 归一化前 L2 范数 → 单位化
  → FaceDetectionResult{faces[i].feature(512, 归一化), bbox, landmarks, score, raw_l2_norm}
```

失败语义：整体失败（未初始化/解码失败/检测链 rknn 调用失败）返回空结果；单脸对齐/特征提取失败仅剔除该脸（较外部 exe 的"任一脸失败整图退出"放宽，贴边小脸不阻断整批）。

### FaceRecognitionWrapper（对照链路）

```
setExecutablePath/setDetectionModel/setRecognitionModel/setThresholds
detectAndExtract(image_path)                      // 同步阻塞
  → fork+execv: <exe> <det> <rec> <image> <image_faces.json> <det_th> <nms_th>
  → parseJsonResult(image_path + "_faces.json")   // 非法 bbox 逐脸剔除
calculateSimilarity(featA, featB)                 // 单位向量点积 = 余弦, O(512)
```

两条链路的特征均为 L2 归一化（`feature_normalized=true`），`raw_l2_norm` 是归一化前范数，可作特征质量参考。

## 使用方法

```cpp
#include "in_process_face_recognizer.h"

InProcessFaceRecognizer rec;
rec.init("/ws/model/face/detection.rknn", "/ws/model/face/recognition.rknn", RKNN_NPU_CORE_AUTO);
rec.setThresholds(0.6f, 0.4f);            // 默认即 0.6/0.4：点名场景宁缺毋滥
FaceDetectionResult res = rec.detectAndExtract("/data/capture/snap_001.jpg");
for (const auto& f : res.faces) {
    // f.bbox 原图坐标；f.feature 为 512 维单位向量
    float sim = FaceRecognitionWrapper::calculateSimilarity(f.feature, galleryFeature);
}
```

## 依赖关系

- 依赖：`rknnrt`（librknn_api）、OpenCV（imread/warpAffine/SVD）、`nlohmann/json`（仅 wrapper 的 JSON 解析）；模型两件套 `model/face/{detection,recognition}.rknn`；
- 被依赖：`src/biz/service/roll_call_service`（点名注册/注销比对，经 `InProcessFaceRecognizer`）；`tests/test_rollcall`（同时用两条链路做对照回归）。

## 注意事项

- **模型配套**：特征语义与维度（512）由 recognition.rknn 决定，换识别模型后旧注册特征全部失效；`tools/face_recognition/main.cpp` 与本实现是同一条推理链的两个载体，**exe 侧推理逻辑若有修改须同步移植**，否则对照回归（`test_rollcall`，基准：同名脸特征余弦 > 0.99）会首先失败。
- **线程安全**：`InProcessFaceRecognizer` 的 rknn 上下文非线程安全，同一实例只允许单线程串行驱动（当前由 RollCallService 串行调用）；核分配用 `RKNN_NPU_CORE_AUTO`，点名为低频批量操作，不参与盘点（核0/1）/报警主链路的固定核预算。
- **wrapper 协议耦合**：exe 命令行 7 参数按位置解析、JSON 字段名硬约定，两侧任一改动必须同步；输出文件名 = `image_path + "_faces.json"`，同一图片路径并发识别会互相覆写，且解析后 `unlink` 被注释（保留调试现场），临时 JSON 会残留在图片目录。
- **wrapper 失败语义**：exe 失败/文件缺失 → 返回空结果；但 JSON 合法而缺必需字段（`image`/`num_faces`/`faces`）时 nlohmann `operator[]` 抛异常未被捕获，会向调用方穿透（详见 .cpp 注记）。
- 相似度只有在两侧特征均归一化时才是余弦（值域[-1,1]）；维度不等/为空返回 0.0f，与"正交"不可区分，先判 `feature.empty()`。
