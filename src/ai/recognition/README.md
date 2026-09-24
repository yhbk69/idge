# src/ai/recognition

> 所属域：**ai 推理与模型应用域**（目录重组 S4c 迁入）

## 功能概述

人脸识别（1:1 比对/点名特征提取）的进程级包装层。本目录不做任何推理：通过 `system()` 拉起**外部识别 exe**（内部完成人脸检测、对齐、512 维特征嵌入，RKNN 工具链独立开发），以**临时 JSON 文件**作为 IPC 取回结构化结果，并提供归一化特征其余弦相似度比对算子。

## 文件清单

| 文件 | 职责 |
|---|---|
| `face_recognizer.h` | 数据结构（`DetectedFace`/`FaceDetectionResult`）与 `FaceRecognitionWrapper` 接口声明 |
| `face_recognizer.cpp` | 命令行拼接与执行、nlohmann JSON 解析（bbox/landmarks 双格式兼容、非法框过滤）、点积=余弦的相似度计算 |

## 核心类与数据流

```
调用方（点名/抓拍比对业务）
  setExecutablePath/setDetectionModel/setRecognitionModel/setThresholds
  detectAndExtract(image_path)                    // 同步阻塞
    → <exe> <det_model> <rec_model> <image> <image_faces.json> <det_th> <nms_th>
    → system() 等待子进程退出码 0
    → parseJsonResult(image_path + "_faces.json")
        num_faces / faces[]: face_id, score, bbox(x1y1x2y2 或 bbox 数组),
        landmarks(嵌套对或扁平对), raw_l2_norm, feature[512]
  calculateSimilarity(featA, featB)               // 单位向量点积 = 余弦, O(512)
    → 与阈值比较完成 1:1 判决
```

exe 侧特征为 L2 归一化（`feature_normalized=true`），`raw_l2_norm` 是归一化前范数，可作特征质量参考。

## 使用方法

```cpp
#include "face_recognizer.h"

FaceRecognitionWrapper rec;
rec.setExecutablePath("/app/bin/face_recog");
rec.setDetectionModel("/app/models/scrfd_det.rknn");
rec.setRecognitionModel("/app/models/arcface_rec.rknn");
rec.setThresholds(0.6f, 0.4f);          // 默认即 0.6/0.4：点名场景宁缺毋滥

FaceDetectionResult res = rec.detectAndExtract("/data/capture/snap_001.jpg");
for (const auto& f : res.faces) {
    // f.bbox 原图坐标；f.feature 为 512 维单位向量
    float sim = FaceRecognitionWrapper::calculateSimilarity(f.feature, galleryFeature);
}
```

## 依赖关系

- 依赖：外部识别 exe（命令行位置参数与 JSON schema 是强耦合契约）、`nlohmann/json`、OpenCV 头（cv::Rect/cv::Size）、libc `system/stat/unlink`；
- 被依赖：点名/考勤与抓拍比对业务（`src/ui/form`、`src/biz/service` 层）。

## 注意事项

- **协议耦合**：exe 命令行 7 参数按位置解析、JSON 字段名硬约定；两侧任一改动必须同步，否则是静默错结果而非编译错误。
- **并发覆写**：输出文件名 = `image_path + "_faces.json"`，同一图片路径并发识别会互相覆写/读错结果；调用方须错峰或保证路径唯一。
- **失败语义不统一**：exe 失败/文件缺失 → 返回空结果；但 JSON 语法合法而缺必需字段（`image`/`num_faces`/`faces`）时 `operator[]`+`get<T>()` 抛 nlohmann 异常**未被捕获**，会向调用方穿透（详见 .cpp 注记）。
- **system() 判断**：`ret == 0` 是 wait 状态字为 0（正常退出且 code 0），不区分崩溃/信号原因；调用全程阻塞，禁止在 UI/解码关键线程直接调用。
- **临时文件残留**：解析后 `unlink` 被注释（保留调试现场），长期运行需在抓拍目录定期清理。
- 命令为裸字符串拼接、无 shell 转义：路径只能来自可信配置，禁止拼接用户可控文本（命令注入面）。
- 相似度只有在两侧特征均归一化时才是余弦（值域[-1,1]）；维度不等/为空返回 0.0f，与"正交"不可区分，先判 `feature.empty()`。
