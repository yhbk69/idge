# 3rdparty/zlmediakit

## 功能概述

ZLMediaKit 流媒体服务的 **C API 内嵌构建产物**（`libmk_api.so` + 头文件 + 一个留作对照的
独立 `MediaServer` 二进制）。工程内由 `src/media/server/http_file_service.*` 单例在
**同进程**内调用（不是 fork 出 MediaServer）。

**当前唯一用途 = http 静态文件服务**：报警上行的 `url` 档需要"把 `data/alarms/yyyyMMdd/`
下的抓拍图与 `.mp4` 片段按路径暴露给对端下载"，本库就是那台服务器。
历史上它承担过 RTSP/HLS/fMP4 实时分发与国标 RTP 发流，那条链路已随
`src/media/server/zlm_service.*` 一并移除（commit f775108），下面的能力表保留是为了
"将来要用时不必重新实测哪些能力是编译期就没了的"。

## 文件/子目录清单

| 路径 | 说明 |
|---|---|
| `include/mk_*.h` | 纯 C 头（只依赖 stdint/stddef），入口聚合头是 `mk_mediakit.h`。**工程内只允许 `src/media/server/` include 它**，避免 `mk_*` 全局符号散落各处 |
| `aarch64/libmk_api.so` | 板端（aarch64）动态库，根 CMake 经 `include_directories` + `link_directories` + `mk_api` 链接；**构建树内靠 `CMAKE_BUILD_RPATH` 直接找到它**（`run.sh` 的 `LD_LIBRARY_PATH` 里并没有本目录，也不需要） |
| `MediaServer` | 官方独立可执行服务器（同版本）。**本工程不启动它**，见"注意事项"互斥条目 |
| `LICENSE` | MIT（ZLMediaKit 项目作者保留所有权利声明，条款见文件） |

## 版本与构建配置（从库内字符串实测，非文档抄录）

```
ZLMediaKit(git hash:e628cba/2023-08-14T00:05:32+08:00, branch:master, build time:2023-08-16T16:40:16)
```

编译期能力（决定"哪些 API 调了也没用"，全部按 `strings libmk_api.so` 核对）：

| 能力 | 状态 | 说明 |
|---|---|---|
| **http 静态文件服务** | ✅ | **本轮唯一用到的能力**，实测语义见下一节 |
| RTSP / HLS / fMP4 分发 | ✅ 库支持 | 工程当前不用（分发链路已移除） |
| FLV(RTMP 格式) over HTTP | ⚠️ H.265 实测 0 帧 | 不起 RTMP 服务 |
| `ENABLE_WEBRTC` | ❌ 未开 | `mk_rtc_server_start` 恒返回 0 并打"未启用webrtc功能" |
| HTTP API (`/index/api/*`) | ❌ 未编译（库内无 `api.*` 配置键） | 2026-10-29 用 `curl /index/api/...` 复验：**404**，与静态文件不抢路径。报警媒体不靠 `getMediaList` 之类的库接口 |
| RTP 发流（国标媒体侧） | ✅ `mk_media_start_send_rtp` | 当前 `src/biz/sip` 不调它（`grep mk_ src/biz/sip` 为空），恢复分发或改国标媒体侧时才用得上 |
| `mk_rtp_server` | ✅ 但语义是**接收** | 平台侧收流用；设备端信令 ZLM 不管，须自实现 |

## 静态文件服务的实测语义（2026-10-29，对本机真起的服务 curl 逐条核对）

换 ZLM 版本必须重跑这几条——它们是报警上行 `url` 档的地基。

| 请求 | 实测 |
|---|---|
| `.jpg` | `200` + `Content-Type: image/jpeg; charset=utf-8`，字节与盘上文件 sha256 一致 |
| `.mp4` | `200` + `Content-Type: video/mp4; charset=utf-8` |
| `.h265` 裸流 | `Content-Type: text/plain` → **播放器不认，所以报警片段对外只给 `.mp4` 副本地址** |
| `Range: bytes=0-1023` | `206 Partial Content` + `Content-Range: bytes 0-1023/<总长>`，总长与 JSON 声明的 `total_bytes` 相等（网页能拖进度条） |
| `GET /`、`GET /<日期>/`（`http.dirMenu=0`） | `404`：目录索引确实关得掉 |
| `%2e%2e%2f` 与裸 `../` 穿越 | `404`。**这是零鉴权服务唯一的目录逃逸防线，必须实测** |
| `/index/api`、`/live/ch1`、`/hls` | 全 `404`，不与静态文件抢路径 |

## 使用方法

```bash
# 链接配置见根 CMakeLists.txt 四处：include_directories(3rdparty/zlmediakit/include)、
# CMAKE_BUILD_RPATH、target_link_directories(3rdparty/zlmediakit/aarch64)、target_link_libraries(mk_api)。
# 构建树（build/build_rk3588_linux/idge）无需额外设置：CMAKE_BUILD_RPATH 已收录本目录与
# 3rdparty/openssl1.1.0/lib（libmk_api.so 自身 NEEDED libssl.so.1.1）。
# 只有脱离仓库跑（或手工调试）时才要补：
export LD_LIBRARY_PATH="$PWD/3rdparty/zlmediakit/aarch64:$LD_LIBRARY_PATH"
```

内嵌调用的最小顺序（完整契约、装配位置与生命周期见 `src/media/server/README.md`）：

```cpp
// 进程级**只允许一次**；stop() 之后本进程不支持再 start
mk_env_init1(2 /*io 线程*/, 3 /*Warn*/, LOG_CONSOLE, nullptr /*不落独立文件*/,
             0, 0, nullptr, 0, nullptr, nullptr);   // 签名见 include/mk_common.h:100
mk_set_option("http.rootPath", "/abs/path/data/alarms");   // 必须绝对路径
mk_set_option("http.dirMenu", "0");                 // 关目录索引
mk_set_option("http.keepAliveSecond", "5");
uint16_t port = mk_http_server_start(8090, 0);      // ssl=0 明文；返回 0 = 起口失败
... 用 mk_get_option 回读一遍再打日志（未知键静默忽略，只有回读能看出来）
mk_stop_all_server();                               // 只在 main 事件循环之后调一次
```

**本轮用到的配置键**（`mk_set_option` 对未知键**静默忽略**、不报错，所以键名只能对照库内字符串；
`config.ini` 示例里的名字不算依据——曾按 `http.directory` 找，库里根本没这个键，真名是 `http.dirMenu`）：

`http.rootPath`、`http.dirMenu`、`http.keepAliveSecond`

库里另外几个对"明文零鉴权服务"有用的键，**当前没设**，需要时按同样方式实测后再加：
`http.allow_ip_range`（限来源网段）、`http.notFound`（自定义 404）、`http.maxReqSize`、
`http.sendBufSize`、`http.charSet`、`http.allow_cross_domains`、`http.virtualPath`。

## 依赖关系

- 上游消费方：**只有 `src/media/server/http_file_service.cpp`**（起停与地址拼装）。
  它的调用方是 `src/ui/form/frmmain.cpp`（起）与 `src/main.cpp`（停）；
  报警侧只拿一个 `baseUrl` 字符串，biz 域不 include 本目录任何头。
- 库自身依赖：`libssl.so.1.1`/`libcrypto.so.1.1`（本仓 `3rdparty/openssl1.1.0/lib` 已自带，
  进 RPATH 即可）+ libatomic + pthread；**不需要** ffmpeg。
  注意工程同时链接了系统 OpenSSL 3（Qt 用），两套版本共存靠 RPATH 顺序，
  这也是 `wss://` 一直不可用的根因（见 `plan/roadmap.md` §十 T-1）。

## 注意事项

- **`mk_env_init1` 是进程级单例、`stop()` 后不能再生**：所以"设置页改了端口就重开服务"做不到，
  端口/开关变更 = 重启进程。这一对在工程里被故意拆开（frmMain 起、main 清理段停），
  别"顺手"合并。
- **与独立 `MediaServer` 进程互斥**：两者抢同一组端口。现场若有人单独跑了本目录的 `MediaServer`，
  内嵌服务的 `start()` 会因 bind 失败返回 false（日志当场打端口号）。
- **默认端口不是 80**：板端以 uid 1000 运行 + `net.ipv4.ip_unprivileged_port_start=1024`
  → 特权口 bind 失败。工程默认 `8090`；要用 80/443 需 root / `setcap cap_net_bind_service=ep`
  / `sysctl -w net.ipv4.ip_unprivileged_port_start=80`。
- **`http.rootPath` 默认值是"可执行文件所在目录/www"**：不显式设就变成"URL 全 404 但本地文件明明在"。
  传相对路径同样会退回这个默认值——`RuntimePaths::alarmsDir()` 返回的是随 cwd 变的相对路径，
  **必须先 `QFileInfo(...).absoluteFilePath()`**。
- **明文 + 零鉴权**：`rootPath` 一定在 `data/alarms`，能连通本机的任何人都能按路径取走全部报警媒体。
  防线只有"对端不知道文件名" + `dirMenu=0` + 越界 404，这不是访问控制；
  所以 `alarm_uplink.enabled` 出厂 `false`，开启前必须确认网段隔离。
- **日志级别 0~4 = Trace/Debug/Info/Warn/Error**：工程取 3(Warn)。Info 级会给每个客户端连接/断开
  各打一条，多浏览器标签时会把 idge 自己的检测日志刷掉。
- **IO 线程取 2**：静态文件只服务"报警后取一次图 + 一段 mp4"的突发小流量。
  4 路 1080p/30fps 满负载下 `top -H` 实测这两个 `event poll` 线程 0.0% CPU、`stamp thread` ~2%，
  不影响解码/推理节拍；若将来抢占就降到 1 并更新本行。
- **本目录的 `MediaServer` 二进制仅留作对照**（抓包、验证 ZLM 行为时可临时单独跑），
  主程序不加载、不启动、不安装。
- **部署缺口（尚未修）**：根 CMake 只有一条 `install(DIRECTORY .../ffmpeg-rkmpp/lib/ DESTINATION lib)`，
  **`libmk_api.so` 与 `libssl.so.1.1` 都不会被拷进 `install/rk3588_linux/lib`**，而安装产物的 RPATH 是
  `$ORIGIN/../lib`。因此 `install/` 那套产物目前跑不到报警媒体服务（构建树内没问题，
  本工程链接期带 `--disable-new-dtags`，写出来的是会传递的 DT_RPATH）。
  修法二选一：补两条 `install(DIRECTORY ...)`，或部署时保留 `3rdparty/` 目录并给 `LD_LIBRARY_PATH`。
- 许可证 MIT，对外发布前过法务清单（与 `3rdparty/README.md` 的汇总一致）。
