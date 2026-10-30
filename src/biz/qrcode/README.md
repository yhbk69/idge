# src/biz/qrcode

> 所属域：**biz 业务域**（本轮新增）

## 功能概述
二维码**解码**（ZBar）。原先这段实现长在智能识别页的 `.cpp` 里是个文件内自由函数，
登录页要用就只能复制一份——而"解不出来"有三层原因（本机构建没编进 ZBar / 图里没有码 /
图片根本读不到），两处必须一致地把层次告诉用户，否则现场只会看到"扫不上"三个字。
所以提取成模块。

**本目录只有解码，没有摄像头**。扫码对话框（预览 + 实时扫描 + 确认）在
`src/ui/widgets/qr_scan_dialog.{h,cpp}`——它要 `GLVideoWidget` 与 `FFmpegVideoDecoder`，
放进 biz 就是 **biz → ui 的反向依赖**（biz 层不许 include `src/ui`），所以按依赖方向留在 ui。
本目录因此可以不带 OpenCV/RKNN 头、脱离摄像头离线自测。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `qr_decoder.h/.cpp` | `qrcode::QrHit`（`QRect` + 原文）、`decodeAvailable()`/`unavailableReason()`、三个解码入口 |

三个入口是**一条链的三层**，不是三套实现：

```
scanQrFromRGBA        ← 真正做 ZBar 的唯一一处（解码器映射指针直给的帧）
  ↑ scanQrFromImage        ← QImage（RGB888/JPEG 解码后的任意格式，内部转 RGBA8888）
      ↑ scanQrFromImageFile ← 落盘的码图：无摄像头环境下验证整条解码链
```

## 口径

- **`HAS_ZBAR` 没编进来时不静默返回空**：`decodeAvailable()` 为 false，
  所有入口返回 false 且 `err` 给出中文原因（`unavailableReason()`，含补齐路径），
  调用点共用这一份文案，不许各写一份。
- **命中矩形用 `QRect` 而不是 `cv::Rect`**：唯一消费者是扫码窗格冻结图上定位点选圆点的中心，
  换掉可让本模块不拖进 OpenCV 头（这是它能离线自测的前提）。
- **谁在调 `scanQrFromRGBA`**：窗格打开即扫，解码挂在 `FFmpegVideoDecoder::frameReady` 上、
  用 `DirectConnection` 跑在**解码线程**里（放 GUI 线程会直接卡住画面，智能识别页是四路都在跑时进来的）。
  每 250ms 最多解一帧——实测 150ms 会把预览从 7.7fps 打到 5.1fps，而现场码是静止张贴的，
  识别延迟多 100ms 没人感觉得到。指针就在生产它的线程上读，双缓冲覆盖竞态天然不存在。
- `scanQrFromImageFile` 读不到文件时报**原因**而不是返回空列表——空列表与"没有码"同形，
  会把"路径错了"误判成"码印坏了"（单测里钉了这条契约）。

## 摄像头独占（为什么全程序只有一个扫码器）
扫码取的是 `config.json` 的 `smartRecognition.camera` 设备节点（本板 `/dev/video22`，
留空则自动探测），而**同一设备节点不能被开两次**（`ConfigManager.h` 明写）。
登录页与智能识别页因此共用 `QrScanDialog`，任何时刻只有一个持有者。

实测（本轮板端）：停在登录页时进程打开的 `/dev/video*` 数量为 **0**
（登录页**不常驻预览**，摄像头只在扫码对话框打开期间被占用），
点「扫码登录」后变成 1 且正是 `/dev/video22`（预览 FPS 计数在跑），
关掉对话框又回到 0。这条不变量是"门挡着的时候不碰摄像头"的实现方式。

## 自测
`tests/test_construction_qr.cpp`：用 `segno` 生成含中文的 PNG → `scanQrFromImageFile()`
解出原文 → `ConstructionSession::startLogin()` 通过；另有一条"缺字段的码解得开内容但过不了校验"。
这把"光照/对焦"与"解析/校验"两层彻底分开，失败时能一眼看出是哪一层。
