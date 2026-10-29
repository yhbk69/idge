# src/biz/sip

> 所属域：**biz 业务域**

## 功能概述

GB/T 28181（国标）**设备侧**视频推流。本模块让 idge 作为一个可被国标平台
（如 GB28181-2016 协议的 SIP 服务端 / 各类"雪亮工程""明厨亮灶"接入网关）
纳管的摄像头设备：主动向平台注册、保活、应答目录与设备信息查询，
平台 INVITE 点流时把本机摄像头 / RTSP / RTMP / MP4 转成 **PS 封装的 H.264**
通过 RTP 推过去。

不实现的部分（当前范围之外）：被叫收流、级联目录订阅、报警事件上报（Alarm）、
录像回放/下载（SUMMARY/Playback）、SIP over TCP/TLS、媒体加密。

## 文件清单

| 文件 | 职责 |
|---|---|
| `gb28181_config.h/.cpp` | `Gb28181Config`/`Gb28181PlatformConfig`/`Gb28181StreamConfig` 三个纯数据结构 + 从 `data/config.json` 载入与校验 |
| `gb28181_manager.h/.cpp` | 信令主体：一条 UDP socket + 一条事件线程，REGISTER 摘要认证、Keepalive、MANSCDP 查询应答、INVITE/ACK/BYE 会话管理 |
| `gb28181_media.h/.cpp` | 单路取流与转码线程：v4l2/RTSP/RTMP/MP4 → Annex-B H.264 + 90kHz PTS（尽量走 rkmpp 硬解硬编） |
| `gb28181_rtp.h/.cpp` | `Gb28181PsMuxer`（H.264→PS 打包）与 `Gb28181RtpSender`（UDP / TCP-RFC4571 发送） |
| `gb28181_ssrc.h` | 国标十进制 SSRC 字符串的解析、合法性判定与 10 位补零 |

## 核心类与数据流

```
main.cpp
  initializeMediaRuntime()                      # avdevice_register_all + avformat_network_init
  Gb28181Config::load(configPath, &probe)       # 判据：serverHost 非空且至少一条流有 deviceCode
        └─ 通过 → Gb28181Manager::start(configPath)   # 否则不启动，主程序照常跑

Gb28181Manager（std::thread eventLoop，200ms 一轮 select）
  start() → openSocket() 绑 localSipPort → 每条 enabled 流建一个 Runtime{media,rtp,muxer}
  每轮：now ≥ nextRegisterMs → REGISTER（首轮无认证，401 带回 nonce 后带 Digest 重发）
        registered 且 now ≥ nextHeartbeatMs → MESSAGE Keepalive
  收包 handleDatagram：
    SIP/2.0 响应  → handleResponse   （401 取 nonce/realm/qop；200 置 registered）
    请求          → handleRequest
        MESSAGE   → 解析 <CmdType>，DeviceInfo / Catalog / DeviceStatus 回 200 + MANSCDP Response
        INVITE    → 取 SDP 的 c=/m=video/y=，定 remoteHost+remotePort+udp|tcp、协商 SSRC
                    （y= 非法回 488）→ 100 Trying → 200 OK（SDP：m=video 9 ... 96、a=sendonly、
                    a=rtpmap:96 PS/90000、y=10位SSRC）→ startMedia()
        ACK       → 无对应分支（不处理；推流在 200 OK 之后就已经开始）
        BYE       → stopMedia()，清 callId，等平台下次 INVITE 重新起流

Gb28181MediaSource（每路一条 std::thread，run()）
  开流（camera: v4l2 + input_format=mjpeg；rtsp: rtsp_transport=tcp）→ 找视频流
    ├─ 源已是 H.264 → h264_mp4toannexb 直通（width/height/fps/bitrate 全部不生效）
    └─ 否则 h264/hevc/mjpeg_rkmpp 硬解（建 rkmpp hwdevice 失败退软解；运行中
          DRM_PRIME 同步失败再中途换软解）→ swscale 到目标尺寸
          → h264_rkmpp 硬编（打不开退 libx264 veryfast+zerolatency）
  产出 Gb28181EncodedFrame{ Annex-B, pts90k, keyFrame } → frameCallback
  首 5 秒 PTS 按墙钟 sleep_until 逐帧限速，之后按解码速度直发

Gb28181PsMuxer::writeVideoFrame → callback → Gb28181RtpSender::sendPs
  缓存 SPS/PPS；等齐"SPS+PPS+IDR"才开始发（不半截起流）
  关键帧载荷 = SPS+PPS+IDR，PS 包带 PSM；普通帧只装 slice
  PS Pack Header(SCR=PTS) + PES(stream_id 0xE0, 仅 PTS)
  切成 ≤1400 字节 RTP 载荷：PT=96、末片 marker=1、seq 每次 open 随机起
  UDP 直接 sendto；TCP 前置 2 字节大端长度（RFC4571）并 send 全量重试
```

## 配置（`data/config.json`）

`ConfigManager` **不读这一段**，由 `gb28181_config.cpp` 自己解析（见 `src/base/config/README.md`）。
两种布局都认：新式包在 `gb28181` 里，或把 `platform`/`streams` 直接放在顶层（老 demo 的写法）。

```json
{
  "gb28181": {
    "platform": {
      "localHost": "0.0.0.0",
      "localSipPort": 5060,
      "advertiseHost": "192.168.1.60",
      "serverHost": "192.168.1.100",
      "serverPort": 5060,
      "serverCode": "34020000002000000001",
      "serverDomain": "3402000000",
      "password": "12345678",
      "registerExpires": 3600,
      "heartbeatInterval": 30
    },
    "streams": [
      { "type": "camera", "cameraIndex": 0, "deviceCode": "34020000001320000001",
        "deviceName": "前门", "width": 1920, "height": 1080, "fps": 25, "bitrate": 2000000 },
      { "type": "rtsp", "source": "rtsp://192.168.1.200/cam1",
        "deviceCode": "34020000001320000002" }
    ]
  }
}
```

### platform 键
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `localHost` | string | `0.0.0.0` | SIP UDP 绑定地址，必须是 IPv4 字面量（或 `0.0.0.0`） |
| `localSipPort` | int | `5060` | 本机 SIP 端口，1~65535 整数 |
| `advertiseHost` | string | 取 `localHost` | 写进 Via/Contact 的对外地址；**不允许 `0.0.0.0`**，绑通配时必须显式填 |
| `advertiseSipPort` | int | 取 `localSipPort` | |
| `serverHost` | string | 无（空则不启用） | 平台 SIP 地址，**只认 IPv4 字面量**，填域名会静默不发注册 |
| `serverPort` | int | `5060` | |
| `serverCode` / `serverDomain` | string | 无 | 平台 20 位编码 / 域；`serverDomain` 缺省时用 `serverHost` |
| `password` | string | 无 | 注册 Digest 口令，可被 stream 级覆盖 |
| `transport` | string | `udp` | **假开关**：填 `tcp` 只多一条告警，SIP 恒走 UDP |
| `registerExpires` | int | `3600` | 注册有效期（秒），实际提前 `expires-15s` 续注 |
| `heartbeatInterval` | int | `30` | Keepalive 间隔（秒），注册成功后才开始 |
| `userAgent` | string | `idge/GB28181` | |

### streams[] 键
| 键 | 类型 | 默认 | 说明 |
| --- | --- | --- | --- |
| `type` | string | `camera` | `camera` / `rtsp` / `rtmp` / `mp4` |
| `source` | string | 空 | 取流地址；`camera` 可留空改用 `cameraIndex` |
| `deviceCode` | string | 空 | 通道国标编码，**空则这条流被直接丢弃** |
| `channelCode` / `serverCode` / `password` | string | 回退上层 | |
| `deviceName` / `serialNumber` | string | `deviceName` 缺省取 `channelCode`；`serialNumber` 缺省空 | 目录/设备信息里回报给平台的名字与序列号 |
| `ssrc` | string | 空 | 备用 SSRC；INVITE 带合法 `y=` 时以平台给的为准 |
| `transport` | string | 回退 platform | **由 INVITE 的 SDP 决定实际传输**，此键基本不起作用 |
| `cameraIndex` | int | `-1` | `type=camera` 且 `source` 空时拼 `/dev/video{index}`，`-1` 视作 0 |
| `width`/`height`/`fps`/`bitrate` | int | 1920/1080/25/2000000 | **只在需要重编码时生效**；源本身是 H.264 走透传时一个都不看。`camera` 的 `fps` 会被探测到的真实帧率覆盖 |
| `enabled` | bool | `true` | false 的流不注册不推流 |
| `loop` | bool | `true` | 只对 `type=mp4` 有意义 |

## 使用方法

接入即"改配置 + 重启"，界面上没有国标开关页（本轮明确不做 GB28181 UI）：

```bash
# 1. 往 data/config.json 加 gb28181 段（照上面模板）
# 2. 重启主程序，看日志
./run.sh 2>&1 | grep GB28181
#   GB28181 media opened / GB28181 RTP PS sent / GB28181 INVITE ...
# 3. 平台侧查设备是否在线，再点播该 deviceCode
```

没有 `gb28181` 段时 `Gb28181Config::load()` 返回 false，`main()` 直接跳过启动，
不会打印错误——"平台看不到设备"第一件事就是确认这一段存在且 `serverHost`/`deviceCode` 都在。

单独接代码（不使用配置文件时）：

```cpp
Gb28181Manager manager;
if (!manager.start("/path/to/config.json")) { /* 失败自带 qWarning */ }
// ... 运行期 running() 只反映事件线程，不代表注册成功
manager.stop();      // 析构会自动调
```

## 依赖关系

- **FFmpeg**：libavformat/libavcodec/libavdevice/libswscale/libavutil（取流、解码、缩放）
  + `h264_mp4toannexb` BSF；编码器 `h264_rkmpp`，退档 `libx264`。
  依赖 `src/media/media_runtime.cpp` 在 `main()` 里做的一次进程级注册。
- **Qt**：仅用 `QString`/`QJsonDocument`/`QCryptographicHash`（MD5 摘要认证）/`QRegularExpression`，
  **不是 QObject**，不依赖信号槽与事件循环。
- **POSIX**：`socket`/`select`/`sendto`/`inet_pton`，`std::thread`。
- **不依赖任何 XML 工具**：本模块自带 SIP 头解析（按 `\r\n` 切行 + `QRegularExpression`）。
  base 域曾有的一份 `xml_utils`（`utf8_to_gb2312`/`extractSN`/`extractCmdType`）从未被这里调用，
  已随死代码清理删除——若要按 GB2312 发 MANSCDP，转码得在本模块内自己加。

## 注意事项

- **IPv4 字面量是硬要求**：`serverHost`、`advertiseHost`、以及 INVITE SDP 里的 `c=` 地址
  都走 `inet_pton`，失败时注册/心跳函数**直接 return，不打日志**（`sendRegister`/`sendHeartbeat` 开头）。
  填域名不是"报错"而是"安静地永远上不了线"，排查时先把域名换成 IP。
- **XML 声明与实发编码不一致**：MANSCDP 报文头写 `encoding="GB2312"`，但 `Content-Length`
  按 UTF-8 字节数计算、正文也是 UTF-8。字段全 ASCII 时两者等价、平台能正常解；
  一旦 `deviceName`/`serialNumber` 含中文，平台按 GB2312 解会成乱码。
  接真实平台前先确认这一点，需要时在本模块内做 UTF-8→GB2312 转码（iconv 或查表）后再算 `Content-Length`。
- **SIP 只有一个 socket，多路流共用 localSipPort**：同一台机器上再起一个独立
  `MediaServer`/另一个 idge 实例会抢端口（`SO_REUSEADDR` 对同协议同端口不保证负载分配）。
- **与本机其它取流方无互斥**：`type=camera` 会自己开一次 v4l2，和监控页的
  `FFmpegVideoDecoder`、智能识别页与点名/盘点的 `CameraPreviewDecoder`
  （后两者统一取 `smartRecognition.camera`）之间没有任何设备注册表。
  同一设备被两处打开，后开的那个失败。
  正式部署建议国标只推 RTSP 拉流，本机摄像头留给业务页。
- **透传路径无视分辨率**：源是 H.264 时不重编，平台看到的就是源本身的宽高码率；
  想靠 `width/height/bitrate` 限带宽，必须让源不是 H.264（或换 `camera`/`rtmp`）。
- **`psFrames`/`rtpSendErrors` 是普通 `uint64`**：媒体线程自增、SIP 线程在
  `startMedia`/`stopMedia` 读，非原子，只够用来打日志，别当判决条件。
- **TCP 会话一旦发送失败就关 socket**：`sendPs` 在 TCP 下失败会主动关闭连接，
  manager 随即 `requestStop()` 停媒体源，等平台下一次 INVITE 重建；
  UDP 丢包则只记 errno 继续发。
- **BYE 后配置仍在**：只停媒体，不删 Runtime，平台可以反复 INVITE。
  但 `stop()` 之后想再 `start()` 需要重新读配置文件。
- **首帧有最长 5 秒等待**：媒体源开头按墙钟限速，平台点流后画面"慢半拍"属预期。
- 编译进主程序：根 `CMakeLists.txt` 用 `file(GLOB src/*/*/*.cpp)` 收本目录全部 .cpp，
  新增文件不用改构建脚本，include 路径已加 `src/biz/sip`。
