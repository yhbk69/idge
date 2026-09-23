# 3rdparty

## 功能概述

Rockchip（rknn_model_zoo 系）交叉编译第三方预编译依赖集合，供根 `CMakeLists.txt` 与 `3rdparty/CMakeLists.txt` 按 `TARGET_SOC`（rk3588）/架构（aarch64）选取头文件与库。覆盖：NPU 推理运行时、2D 加速、媒体硬解硬编、流媒体、图像/视频编解码、JSON、加密、消息队列、语音特征等。

## 文件/子目录清单

| 目录 | 版本（头文件实测） | 用途 | 工程接入方式 |
|------|--------------------|------|--------------|
| `rknpu2/` | librknnrt.so（含 `rknn_api.h`） | RK3588 NPU 推理运行时（3 核 18 TOPS） | `LIBRKNNRT=rknpu2/Linux/aarch64/librknnrt.so`，链接 `rknnrt`，RPATH 收录 |
| `rknpu1/` | — | 老平台（rk1808/rv1126 等）librknn_api，rk3588 不用 | 仅 `TARGET_SOC` 匹配 rknpu1 系时生效 |
| `librga/` | im2d API **1.10.0[2]**（`include/im2d_version.h`） | RGA 2D 加速（色彩转换/缩放/填充） | `LIBRGA=Linux/aarch64/librga.a`+`rga` 链接名、include 路径 |
| `mpp/` | librockchip_mpp.so.1 | MPP 媒体硬件编解码平台 | include；由 ffmpeg-rkmpp 间接使用 |
| `ffmpeg-rkmpp/` | avcodec **60.31.102**、avutil 58.x（**FFmpeg 6.1 线 + rkmpp 补丁**），含 bin/ffmpeg、ffprobe | 硬解/硬编（h264_rkmpp/hevc_rkmpp/rga 滤镜） | include+link 该 lib，RPATH 收录，install 时拷贝 .so |
| `ffmpeg/` | avcodec 60.x（普通构建） | 备用 FFmpeg，**当前 CMake 未引用** | 无 |
| `opencv/` | **3.4.5** 静态库（`opencv-linux-aarch64/lib/libopencv_*.a`；另有 armhf/android 版） | 图像处理 | ⚠️ 实际未生效，见"注意事项"第 1 条 |
| `jsoncpp/` | **1.9.7**（`include/json/version.h`） | 配置文件 JSON 解析 | include + `jsoncpp` 链接（libjsoncpp.so.1.9.7） |
| `sqlite/` | **3.49.1** amalgamation（`sqlite/sqlite3.c/h`） | SQLite 源码包 | 本目录有独立 CMakeLists，但根工程未 `add_subdirectory`；主程序链接的是**系统 sqlite3** |
| `zlmediakit/` | C API（`include/mk_*.h`）+ `aarch64/libmk_api.so` + `MediaServer` 二进制 | 流媒体协议（RTSP/FLV 等） | include + `mk_api` 链接，RPATH 收录 |
| `jpeg_turbo/` | libturbojpeg.a（Linux/{aarch64,armhf,...}） | JPEG 硬加速解码 | `LIBJPEG`+`LIBJPEG_INCLUDES` 经 3rdparty/CMakeLists 传递 |
| `libyuv/` | libyuv.a | CPU 色彩转换备选 | link_directories 收录 |
| `openssl1.1.0/` | **1.1.0l**（2019-09-10） | TLS 头文件路径（实际链接系统 crypto/ssl） | include 路径 |
| `openssl3.0.21/` | **3.0.21** | 新版 OpenSSL 备用套件，CMake 未引用 | 无 |
| `zmq/` | **4.3.5**（zmq.h） | ZeroMQ 消息队列（备用） | 未链接进主程序 |
| `kaldi_native_fbank/`、`fftw/`、`libsndfile/` | — | 语音特征链路（FBank/FFT/音频解码），3rdparty/CMakeLists 仅对 rv1106/rv1103 用自带静态库，其余用系统 so | rk3588 下不链接 |
| `allocator/` | drm/、dma/ 头 | DMA-BUF/DRM 分配器（零拷贝），配合 `ENABLE_DMA32`→`-DDMA_ALLOC_DMA32` | include 路径 |
| `timer/` | `easy_timer.h` | 毫秒计时工具（CLI FPS 统计用） | include 路径 |
| `stb_image/` | stb_image.h / stb_image_write.h | 单头图像读写 | `STB_INCLUDES`（注意根 CMake 里 `3rdparty/stb` 死路径，见注意事项） |
| `opencl/libopencl-stub/` | — | OpenCL 加载桩（备用） | 无 |
| `CMakeLists.txt` | — | 按 SOC/架构导出 LIB* 变量的总控脚本 | 被根工程 add_subdirectory |

## 使用方法

```bash
# 唯一真实构建入口（交叉编译 rk3588）：
./build-linux.sh -t rk3588 -b Release        # 可选 -m(ASan,需Debug) -d(DMA32) -r(禁用RGA)
# 产物：build/build_rk3588_linux/idge，install 目录 install/rk3588_linux
```

`3rdparty/CMakeLists.txt` 依据 `TARGET_SOC` 与指针宽度选择 `Linux/aarch64` 等子目录里的预编译 `.so/.a`，向父工程回传 `LIBRKNNRT / LIBRGA / LIBJPEG / STB_INCLUDES ...` 变量；根 CMake 再经 `target_link_directories/target_link_libraries` 链接。

## 依赖关系

- 上游消费方：`src/yolo11`（rknn_api）、`src/rga`（im2d）、`src/reader`（avformat/avcodec rkmpp）、`src/buffer`（allocator/dma）、`src/config`（jsoncpp）、`src/RtspWorker`/流处理（mk_api）；
- 本目录自身不依赖工程代码，纯被依赖方；
- 运行期还依赖板端系统库：libdrm、libEGL/libGLESv2、libasound/Qt 多媒体后端。

## 注意事项

- **OpenCV 版本不一致（已核实）**：根 `CMakeLists.txt` 第 114 行 `set(OpenCV_DIR ${CMAKE_SOURCE_DIR}/3rdparty/opencv4.10/lib/cmake/opencv4)`，但仓库自带的是 `3rdparty/opencv/opencv-linux-aarch64`（**OpenCV 3.4.5**），`3rdparty/opencv4.10` 目录**不存在**。因此 configure 阶段依赖构建机上另行放置的 OpenCV 4.10 交叉套件，仓库自含 3.4.5 实际未被使用；同时第 153 行备用 include 路径 `3rdparty/opencv/include/opencv4` 也不存在（真实路径是 `opencv/opencv-linux-aarch64/include/opencv2`）。复现构建时需自行补齐 opencv4.10 或改指自带版本（3.4.5 与 4.x API 不兼容，勿随意切换）。
- 根 CMake 的 `3rdparty/librga/lib` 与 `3rdparty/stb` 两个路径**不存在**（rga 实际链接名/`${LIBRGA}`、stb 经 `STB_INCLUDES` 指向 `stb_image`），属冗余死路径，遇到 -lrga 解析失败时优先检查系统库目录。
- 预编译库均为 aarch64（部分含 armhf/Android 多套），**不可在本机 x86 直接链接**；换 SoC 需同步核对 `3rdparty/CMakeLists.txt` 的 SOC 分支。
- `install(PROGRAMS ${LIBRKNNRT} ...)` 会把 librknnrt.so 拷入 install/lib；部署到板子时注意板上 NPU 驱动版本与 librknnrt 配套（不匹配报 RKNN_ERR_DRIVER_*）。
- zlmediakit/MediaServer 是独立可执行流媒体服务器，不随主程序启动，需要时手动运行。
- 第三方许可证各异（RKNN/RGA Apache-2.0、jsoncpp MIT、zmq MPL、zlmediakit MIT、libyuv BSD 等），对外发布前过一遍法务清单；`librga/README.md`、各 LICENSE 文件保留在原目录。
