# src/media/server

> 所属域：**media 媒体域**

## 功能概述
报警上行的 **url 交付档**用的内嵌 HTTP 静态文件服务：报警媒体（抓拍 `.jpg` 与片段 `.mp4`）
仍然只写在 `data/alarms/yyyyMMdd/` 下，JSON 里发的是这些文件的**地址**，字节由对端自己下载。
本目录就是那台"地址指向的服务器"——**同进程内嵌** vendored ZLMediaKit 的 C API
（`3rdparty/zlmediakit/`），不 fork `MediaServer` 进程、不引入第二个可执行文件。

它只做一件事：把一个目录以明文 http 暴露出去。**没有鉴权、没有 TLS、没有索引**，
所以它的存在本身就改变了本机的网络暴露面。
判据在装配处只有一处：`alarm_uplink.enabled` 且 `media_transport="url"` 才 `start()`，
出厂态（uplink 关）与 `ws_binary` 档**一次都不碰 `mk_*`**。

## 文件清单

| 文件 | 说明 |
| --- | --- |
| `http_file_service.h` | 进程级单例 `HttpFileService`：`start/stop/port/running/baseUrl/firstUsableIpv4`，文件头写死三条库约束 |
| `http_file_service.cpp` | **全工程唯一** include `mk_*.h` 的编译单元；公开接口一律 Qt 类型，`mk_` 的指纹不外泄 |

## 核心类与数据流
- **HttpFileService**（函数内 static 单例，无 Qt 线程亲和：内部只有 C API 调用与 `QNetworkInterface` 查询）。

```
frmMain 构造（配置已 load）
    → alarm_uplink.enabled && media_transport=="url"
        → root = QFileInfo(RuntimePaths::alarmsDir()).absoluteFilePath()   ← 必须绝对化
        → HttpFileService::instance().start(root, cfg.alarmUplinkMediaHttpPort())
            ├─ mk_env_init1(io=2, level=Warn, LOG_CONSOLE, 无文件日志, ini/ssl 全空=库内默认)
            ├─ mk_set_option("http.rootPath" | "http.dirMenu"=0 | "http.keepAliveSecond"=5)
            ├─ qInfo 回读 rootPath / dirMenu（未知键会被静默忽略，只有回读能看出"设了没生效"）
            └─ mk_http_server_start(port, 0) → 返回实际绑定端口，0=失败
        → AlarmUplink::instance().setMediaBaseUrl(svc.baseUrl(cfg.alarmUplinkMediaBaseUrl()))
    → AlarmUplink::instance().attach()          ← 顺序不能倒：attach 里要把 baseUrl 折算进 Settings

main() 事件循环结束之后
    → HttpFileService::instance().stop()        ← mk_stop_all_server()
```

**start 与 stop 故意跨作用域不成对**：`mk_env_init1` 初始化整个库的运行时，
`stop()` 之后本进程**不支持再 start**（实测第二次 `start()` 直接返回 false 并打
"ZLM 运行时已初始化过"）。所以这一对不能"顺手"挪进 `~frmMain`——`frmMain w` 是 `main`
的栈对象，析构点在事件循环之后、清理段之前，挪进去等于把 stop 提前到还没关 WS 的时刻，
而更常见的错法是有人加"设置页改了端口就重开服务"：**做不到，改端口必须重启进程**。

起服务失败（最常见是端口被占，见"注意事项"两条）不把报警上行整体关掉：
`baseUrl` 回空串 → `Settings.mediaBaseUrl` 空 → `alarm_uplink_engine` 的 `urlUsable` 为假
→ 两条媒体腿整体不申报，`alarm_info` 的纯文本照发。这是有意的降级方向，
**宁可少发媒体也不要发一个取不到的地址**。

## 使用方法
`data/config.json` 的 `alarm_uplink` 段（三个键由 `ConfigManager` 的 `alarmUplinkMedia*()` 读取）：

```json
{
  "alarm_uplink": {
    "media_transport": "url",
    "media_http_port": 8090,
    "media_base_url": ""
  }
}
```

| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `media_transport` | string | `"url"` | `"url"` = 本文件的服务 + JSON 发地址；`"ws_binary"` = 字节走 WS 分块帧，**此时本服务整个不起** |
| `media_http_port` | int | 8090 | `qBound(1024, _, 65535)`。板端 `net.ipv4.ip_unprivileged_port_start=1024` 且 uid 1000，**80/443 bind 不上** |
| `media_base_url` | string | 空（自动） | 非空则原样采用（去尾斜杠），用于对端只能经 NAT/DNS 名访问、或本机多网卡要指定对外那张；空 = `http://<第一个非回环 IPv4>:<实际绑定端口>` |

**这三个键在已有现场设备上永远不会自动出现**：`migrateAlarmUplinkDefaults()` 只补缺失段、
不覆盖已有段，老 `config.json` 里的 `alarm_uplink` 段会被原样保留，新键全靠 getter 兜默认值。
要改只能手改 JSON。设置页无控件（`recording`/`alarm_uplink`/本服务都还没做 UI）。

### 实测语义（curl 逐条核对，ZLM 版本换了要重跑）

| 请求 | 实测结果 |
| --- | --- |
| `GET /<日期>/<某张存在的>.jpg` | `200`，`Content-Type: image/jpeg; charset=utf-8`，字节与盘上文件 sha256 一致 |
| `GET /<日期>/<某个存在的>.mp4` | `200`，`Content-Type: video/mp4; charset=utf-8` |
| `HEAD/GET` 带 `Range: bytes=0-1023` | `206 Partial Content` + `Content-Range: bytes 0-1023/<总长>`，**总长与 JSON 里 `total_bytes` 相等** → 网页能拖进度条 |
| `GET /` 与 `GET /<日期>/` | `404`：`http.dirMenu=0` 确实关掉了目录索引，文件名列表不外泄 |
| `GET /%2e%2e%2f%2e%2e%2fetc/passwd` 与裸 `../../` | `404`：路径穿越拦得住。**这是零鉴权服务唯一的目录逃逸防线**，所以必须实测而不是相信库文档 |
| `GET /index/api/...`、`/live/ch1`、`/hls/...` | 全 `404`：本产物未编译 http API 与流媒体前缀，不与静态文件抢路径 |
| `.h265` 裸流 | `Content-Type: text/plain` → **片段地址必须给 `.mp4` 副本**，裸流给出去对端播放器不认 |

## 依赖关系
- `3rdparty/zlmediakit/`：`mk_api` 静态/动态库 + 头文件，`CMakeLists.txt` 四处恢复
  （include 目录、`CMAKE_BUILD_RPATH`、`target_link_directories`、`target_link_libraries`）。
  `libmk_api.so` 自身 NEEDED `libssl.so.1.1`，由 `3rdparty/openssl1.1.0/lib` 进 RPATH 解决
  ——本工程链接期带 `LINKER:--disable-new-dtags`，写出来的是 **DT_RPATH**（`readelf -d` tag `0x0f`），
  会**传递**地满足下游依赖，所以 `run.sh` 不需要额外加 `LD_LIBRARY_PATH`。
- 被 `src/ui/form/frmmain.cpp` 起、被 `src/main.cpp` 停；`AlarmUplink` 只要一个 `baseUrl` 字符串。
- **media 域不 include biz 域**，所以地址换算的 `mediaUrlFor()` 留在
  `src/biz/ws/alarm_uplink_engine.h`，由 `AlarmUplink` 把 `rootDir` 与 `baseUrl` 喂给它，
  本服务不参与任何路径判断。
- 本目录新增文件需根 `CMakeLists.txt` 的 media GLOB 覆盖到（`src/media/server/` 在 GLOB 范围内）。

## 注意事项
- **明文 + 零鉴权 + 监听端口**：一旦 `rootPath` 定在 `data/alarms`，
  **能连通本机的任何人都能按路径取走所有报警媒体**。"对端不知道文件名" + 关掉索引 + 404 兜底
  只是可见性，**不是访问控制**。因此出厂态 `alarm_uplink.enabled=false`；开它之前必须先确认
  网段隔离。库里另有 `http.allow_ip_range` 可限来源网段（默认未设＝不限），`http.notFound` 可换 404 页面。
- **端口/开关改动必须重启进程**（`mk_env_init1` 进程级一次、`stop()` 后不能再生）。
  不提供运行期重开，也不要在设置页里"顺手"加一个重启按钮。
- **`rootDir` 必须是绝对路径**。`RuntimePaths::alarmsDir()` 返回 `data/alarms`，随 cwd 变；
  传相对路径进去 ZLM 会退回它自己的默认根（可执行文件所在目录 `/www`），
  症状是"URL 全 404，但本地文件明明在"。
- **`mk_set_option` 对未知键静默忽略**：键名只能对照库内字符串，`config.ini` 里的名字不算依据。
  本轮踩过的坑：目录索引的键是 `http.dirMenu`，**不是** `http.directory`。
  新增任何键都要跟着那行 `qInfo` 回读一起验证。
- **与独立 `MediaServer` 互斥**：现场若有人单独跑了 `3rdparty/zlmediakit/MediaServer`，
  本服务 `start()` 会因端口被占返回 false，日志当场打出端口号（`起口失败 port= …`）。
- **IO 线程取 2，实测不抢节拍**：`top -H` 下 ZLM 的两个 `event poll` 线程 0.0% CPU、
  `async log` 0.0%、`stamp thread` 约 2%（4 路 1080p/30fps 满负载 + 连续报警期间采样），
  `paintGL` 无 >200ms 尖峰、`arrivals/s` 仍 29.8~30.2。静态文件只服务"报警后取一次图和一段 mp4"
  这种突发小流量，2 个线程够；若将来实测发现抢占 NPU/解码就降到 1 并更新这行数字。
  日志级别取 Warn：Info 会给每个连接/断开各打一条，多开几个浏览器标签就把检测日志刷没了。
- **地址是 best-effort**：`data/alarms` 由 `AlarmManager::pruneAlarmMedia` 按天删 + 4GB 硬上限清理，
  **上限能删掉当天的整个目录**（实测有一条 `alarm media over cap, dropped "20261027"`）。
  对端必须"收到 `media_begin` 就立刻取回"，404 是正常终态而不是 bug；本轮不做落库补推。
- **install 产物暂时跑不起来**（已知遗留，未修）：`CMakeLists.txt` 的 install 规则只搬 ffmpeg 的库，
  `install/rk3588_linux/lib/` 里没有 `libmk_api.so` 与 `libssl.so.1.1`，而安装树 RPATH 是 `$ORIGIN/../lib`。
  构建目录内正常。补法是两行 `install(DIRECTORY ...)`，留给部署那一轮。
