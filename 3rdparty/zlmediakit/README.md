# 3rdparty/zlmediakit

## 功能概述

ZLMediaKit 流媒体服务的 **C API 内嵌构建产物**（`libmk_api.so` + 头文件），工程内由
`src/media/server/zlm_service.*` 单例在**同进程**内调用（不是 fork 出 MediaServer）。
用途：把硬件编码出来的 Annex-B 包零转码分发给自研网页（HLS/fMP4）与国标平台（RTSP）。

## 文件/子目录清单

| 路径 | 说明 |
|---|---|
| `include/mk_*.h` | 纯 C 头（只依赖 stdint/stddef），入口聚合头是 `mk_mediakit.h`。**工程内只允许 `src/media/server/` include 它**，避免 `mk_*` 符号散落各处 |
| `aarch64/libmk_api.so` | 板端（aarch64）动态库，根 CMake 经 `link_directories` + `mk_api` 链接；**构建树内靠 `CMAKE_BUILD_RPATH` 直接找到它**（`run.sh` 的 `LD_LIBRARY_PATH` 里并没有本目录，也不需要） |
| `MediaServer` | 官方独立可执行服务器（同版本）。**本工程不启动它**，见"注意事项"互斥条目 |
| `LICENSE` | MIT（ZLMediaKit 项目作者保留所有权利声明，条款见文件） |

## 版本与构建配置（从库内字符串实测，非文档抄录）

```
ZLMediaKit(git hash:e628cba/2023-08-14T00:05:32+08:00, branch:master, build time:2023-08-16T16:40:16)
```

编译期能力（决定"哪些 API 调了也没用"，全部按 `strings libmk_api.so` 核对）：

| 能力 | 状态 | 工程侧处置 |
|---|---|---|
| RTSP / HLS / fMP4 分发 | ✅ | 主用路径（H.265 网页播放走 HLS + fMP4） |
| FLV(RTMP 格式) over HTTP | ⚠️ H.265 实测 0 帧 | 不起 RTMP 服务；地址表标注"仅 H.264 可播" |
| `ENABLE_WEBRTC` | ❌ 未开 | `mk_rtc_server_start` 恒返回 0 并打 "未启用webrtc功能"；`StreamServerConfig` 里**不留 rtc_port**，配置项也已从 config 移除 |
| HTTP API (`/index/api/*`) | ❌ 未编译（库内无 `api.*` 配置键） | 网页拿不到 `getMediaList` / webrtc 信令；报警查询走 `src/biz/upload` 侧的自建 JSON 接口 |
| RTP 发流（国标媒体侧） | ✅ `mk_media_start_send_rtp` | **P5 GB28181 用它做 PS 打包 + RTP 分包**，我们只喂编码包 |
| `mk_rtp_server` | ✅ 但语义是**接收** | 平台侧收流用；设备端信令（注册/心跳/INVITE）ZLM 不管，须自实现 |

## 使用方法

```bash
# 链接配置见根 CMakeLists.txt：include 3rdparty/zlmediakit/include、
# link_directories 3rdparty/zlmediakit/aarch64、target_link_libraries ... mk_api
# 构建树（build/build_rk3588_linux/idge）无需额外设置：CMAKE_BUILD_RPATH 已收录本目录。
# 只有脱离仓库跑（或手工指定 LD_LIBRARY_PATH 调试）时才要补上：
export LD_LIBRARY_PATH="$PWD/3rdparty/zlmediakit/aarch64:$LD_LIBRARY_PATH"
```

内嵌调用的最小顺序（完整契约与线程约束见 `src/media/server/README.md`）：

```cpp
mk_env_init(&cfg);                      // 进程内只一次；stop() 后不支持再启动
mk_set_option("http.rootPath", root);   // 键名必须对照库内字符串，错键静默忽略
uint16_t port = mk_http_server_start(8090, 0);   // 0 = 起口失败（占用/无权限）
mk_media m = mk_media_create("__defaultVhost__", "live", "ch1", 0, 1, 0);
mk_track t = mk_track_create(MKCodecH265, &args);
mk_media_init_track(m, t); mk_track_unref(t);
mk_media_init_complete(m);              // 不调会多等 3 秒
mk_media_input_h265(m, data, size, ptsMs, ptsMs);   // dts==pts（bframes=0）
mk_media_release(m);                    // 必须在创建它的那个线程
mk_stop_all_server();
```

## 依赖关系

- 上游消费方：`src/media/server/zlm_service.cpp`（分发）、`tests/test_stream.cpp`（stage 9 自测）；
  P5 的 `src/biz/gb28181` 将复用 `mk_media_start_send_rtp`。
- 库自身依赖：系统 OpenSSL / libatomic / pthread（板端已具备）；**不需要** ffmpeg。

## 注意事项

- **与独立 `MediaServer` 进程互斥**：两者监听同一组端口，且 `libmk_api.so` 的 `mk_env_init`
  是进程级单例。内嵌跑起来时现场**不能再起** MediaServer，否则端口 bind 失败（表现为分发降级）。
- **默认端口不是 80/554**：板端以 uid 1000 运行 + `net.ipv4.ip_unprivileged_port_start=1024`
  → 特权口 bind 失败。工程默认 `8090/8554`；要用标准口需 root / `setcap cap_net_bind_service=ep`
  / `sysctl -w net.ipv4.ip_unprivileged_port_start=80`。
- **`http.rootPath` 默认值是"可执行文件所在目录/www"**：不改就会把 HLS 切片倒进 `build/`。
  工程显式指到 `data/www`（已 gitignore）。
- **配置键只能对照库内字符串**：`mk_set_option` 对未知键静默忽略，留默认值，
  排查时"看着设了其实没生效"最难查。本工程用到的键：
  `http.rootPath`、`protocol.{enable_rtsp,enable_hls,enable_ts,enable_fmp4,enable_mp4,enable_audio,auto_close,hls_save_path}`、
  `general.streamNoneReaderDelayMS`。
- **日志级别 0~4 = Trace/Debug/Info/Warn/Error**：工程取 3(Warn)。Info 级会给每个客户端连接/断开
  各打一条，4 路 + 多浏览器标签时会把 idge 自己的检测日志刷掉。
- **`auto_close` / `streamNoneReaderDelayMS` 必须置 0**：默认无人观看若干秒即关流，
  与"常驻分发"相反，会让网页第二次拉流多一段起流延迟。
- **本目录的 `MediaServer` 二进制仅留作对照**（抓包、验证 ZLM 行为时可临时单独跑），
  主程序不加载、不启动、不安装。
- **部署缺口（尚未修）**：根 CMake 只有一条 `install(DIRECTORY .../ffmpeg-rkmpp/lib/ DESTINATION lib)`，
  **`libmk_api.so` 不会被拷进 `install/rk3588_linux/lib`**，而安装产物的 RPATH 是 `$ORIGIN/../lib`。
  因此 `install/` 那套产物目前跑不到分发（构建树内没问题）。修法二选一：补 install 规则，
  或部署时保留 `3rdparty/` 目录并给 `LD_LIBRARY_PATH`（见"使用方法"）。
- 许可证 MIT，对外发布前过法务清单（与 `3rdparty/README.md` 的汇总一致）。
