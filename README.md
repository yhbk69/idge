# IDGE - 施工行为监测与分析系统

基于 Rockchip RK3588 边缘计算平台的施工行为监测与分析系统，集成视频解码、YOLO 目标检测、电子围栏、报警管理、人员点名、设备盘点、二维码/铭牌 OCR 识别、GB/T 28181 国标推流和 Qt GUI，用于实时监控与分析施工现场的安全行为。

---

# 一、快速开始（使用方法）

## 1. 一键运行

```bash
/build-linux.sh -t rk3588 -b Release

# 仅做两件事：cd 进 build/build_rk3588_linux，然后执行 ./idge
./run.sh
```

库路径不在这里设置——构建目录里的 `idge` 由 CMake 写入了绝对 RPATH
（`3rdparty/*/lib` 与 Qt5 路径），所以直接跑就能找到 rkmpp FFmpeg、librga、RKNN、ZBar。
装到 `install/rk3588_linux/` 的那份则用 `$ORIGIN/../lib` 相对 RPATH，可整目录搬走。

> **注意**：必须加载 **Mali EGL**（而非 Mesa），否则渲染黑屏。本板靠系统 ldconfig /
> `/usr/lib/aarch64-linux-gnu/mali/` 生效，`run.sh` 并不处理它——若换了显示栈请自行核对
> `ldd ./idge | grep -i egl` 落到哪个实现。

## 2. 工作区目录约定（IDGE_WORKSPACE）

人员点名、设备盘点、智能识别三个业务模块的资源路径以环境变量 `IDGE_WORKSPACE` 为根目录，
**缺省回退到当前工作目录**（见 `src/ui/form/frmmain.cpp`）：

```bash
export IDGE_WORKSPACE=/home/pi/cc/idge_work   # 示例
./run.sh
```

工作区内的目录结构约定：

```
$IDGE_WORKSPACE/
├── model/
│   ├── face/                      # 人员点名权重（两件套 .rknn，进程内推理直接加载）
│   │   ├── detection.rknn         # SCRFD 人脸检测模型
│   │   ├── recognition.rknn       # 人脸特征比对模型
│   │   └── face_recognition       # CLI 对照工具构建产物（点名已进程内化，主程序不再调起）
│   └── library/                   # 模型库：设备盘点权重经 ModelRegistry 从这里解析
│       ├── yolo11n-coco/          # 当前盘点主力权重（明火模型入库后自动加入盘点清单）
│       │   ├── model.rknn
│       │   └── labels.txt
│       └── ppocrv4-ocr/           # 智能识别铭牌 OCR 权重（det/rec + labels）
│           ├── det.rknn
│           ├── rec.rknn
│           └── labels.txt
├── 3rdparty/
│   └── ppocrv4/bin/               # OCR 外部可执行（QProcess 调起，见 3rdparty/ppocrv4/README.md）
└── data/
    └── roll_call_data/            # 点名/盘点业务数据
        └── roll_call.db               # 业务 SQLite 库（任务/人脸/设备表）
```

模型文件缺失时对应业务页初始化会弹窗告警，但视频监控主功能不受影响。
智能识别的权重与可执行分处 `model/`（模型库）和 `3rdparty/`（第三方产物）两处，
缺一即卡片显示"未部署"并短路——判据是五个文件同时存在（`det.rknn`/`rec.rknn`/`labels.txt`
+ 两个 demo），见 `SmartRecognitionWidget::ocrModelsReady()`。

## 3. 主检测配置（config.json）

程序启动时读取 **当前工作目录**下的 `data/config.json`（首次可从 `config_example.json` 复制：`mkdir -p data && cp config_example.json data/config.json`；旧版根目录 config.json 启动时自动迁移），在 **设置页** 修改后自动回写。⚠ `./run.sh` 会先 `cd` 进 `build/build_rk3588_linux`，于是它读的是 `build/build_rk3588_linux/data/config.json`，与源码根的 `data/` 是两份互不相干的副本——"改了配置没生效"先确认改的是哪一份。

**schema V2（按通道 + 按模型）**：配置里不再出现任何 `.rknn`/`labels.txt` 路径，模型的唯一事实源是
`model/library/<库id>/{model.rknn,labels.txt,meta.json}`，配置只用**库目录名**引用它。
老结构（`model.path` + `cascade.models[5]` + `alarm.classes` + `video.channel1..4`/`notes`）由
`ConfigManager::migrateSchemaV2()` 在首次启动时一次性改写并落盘，之后不再触发。关键配置：

| 配置节 | 说明 |
|--------|------|
| `video.models` | 模型参数表，**以库目录名为键**：`{"yolo11n-ppe": {"alarm_class": "un_helmet", "conf_threshold": 0.3, "note": "…"}}`。`alarm_class` 与 `conf_threshold` 都是**这个模型的参数**（不是槽位、不是通道）：类别留空 = 只画框不报警，`conf_threshold` **缺键 = 跟随 `detect.conf_threshold`**（不是 0）。设置页"模型参数"表按行编辑，改完**即时生效**、不重开流 |
| `video.sources` | 数组，**视频流注册表**：一条 = 一路可复用的取流地址 + 它的打开参数（`type`/`url`/`note`/`width`/`height`/`fps`/`inputFormat`/`loop`/`hardwareDecode`/`timeoutMs`/`reconnectMs`，主键 `id`）。同一只摄像头或同一个文件只写一次，**视频通道用 `source_id` 引用它，国标推流的 `gb28181.streams[]` 也从这里取**（2026-11-04 起国标段不再自带 `type`/`source`/宽高帧率），改一处几处一起改；设置页「视频流管理…」编辑这张表。字段口径与默认值见 `src/base/config/README.md` |
| `video.channels` | 数组，**下标即通道号（0-based）**，条目里的 `channel` 字段只给人看。每路 = `source_id`（引用上面那张表）/`source`（内联地址，旧口径，仍支持）/`note`/`enabled`/`models`。**解析只有 `ConfigManager::resolveChannelSource()` 一处**：`source_id` 命中→用那条流；为空→回落内联 `source`；`source_id` 悬空→**不回退**，这一路不开 + 一条 warning。`models` 的顺序就是级联槽位顺序（画面上 `M1/M2/…` 前缀、报警 `result_id`），重排会改号；`enabled=false` 表示这一路不开流。这三项在设置页改完都会**重开该路**（见 §八 第 10 条）；换流用每路的下拉、换本地文件点「浏览」（它会在注册表里建一条 `type=file` 的流并引用），只有直接改 config 文件需要自己动一次该通道配置 |
| `detect` | `conf_threshold`/`nms_threshold` 是**全局底座**（默认 0.25/0.45），推理侧真的读：回落链 `video.models[id].conf_threshold` → `detect.conf_threshold` → 形参默认值，唯一解析点 `ModelRegistry::resolveConfThreshold()`。conf 有**下限 0.05**（`conf=0` 会打穿量化模型的预筛、把推理线程跑死），nms 没有下限。⚠ `class_num`/`threads` 是**假开关**：已连 getter 一起删掉（类别数属于每个模型，推理线程数 = 各通道模型数之和，设置页显示的是真实值），老配置里残留的这两个键程序不再读取 |
| （无 `video.render`） | 推理间隔是编译期的 `frame_count % 3`（`ffmpeg_video_decoder.cpp` 的 4.6 节），**没有配置键**，别在 JSON 里加 `render.sampleInterval`——它只会变成一个改了不生效的假开关 |
| `geofence` | 围栏**不属于级联**：复用该通道已有的检测结果、只做几何判定，不额外占用 NPU。`enabled` 总开关、`alarmClass` **单值触发类别**（默认 `person`，必须是该通道某个模型 `labels.txt` 里的叫法）、`throttleSeconds` 报警**节流下限**（默认 10 秒，每通道**每角色**每类别窗口内只留一条）、`channels` 每路的矩形/多边形顶点（overlay 像素坐标，随存 `overlay_w`/`overlay_h`，改窗口布局会偏移），**每个形状自带 `role`**——`forbidden` 禁区（进去就报）/ `workzone` 岗区域（待在外面就报），同一通道可混用。`mode`（`inside_alarm`/`outside_alarm`）**已退役**：唯一读者是 `FenceManager::loadFromConfig` 里给无 `role` 键的老形状补角色的迁移分支。报警语义是**进入沿**：走进禁区报 1 条、站在里面不动不再报；人在所有岗区域之外报 1 条「离岗」、回岗待 >1.5 秒再出来才算新事件（事件表在解码器 `fenceTracks_`，按角色分池，见 `src/biz/geofence/README.md`） |
| `database` | `storeDetections`（报警相关检测明细是否入 `detections` 表）与 `detectionRetentionDays`（保留天数）；写成 `null` 等于吃默认值 true/30 |
| `recording` | 报警视频硬件编码录制（**默认 `enabled=true`**，报警必带关联片段是需求验收项）；`buffer_seconds` 需 ≥ 片段窗口 3+2 秒，缺键默认 8（`config_example.json` 模板当前写 5） |
| `smartRecognition` | 智能识别四张卡片（二维码/OCR/点名/盘点）共用的取流地址；键缺省时自动探测摄像头节点（MJPEG 优先，其次 CSI/rkisp 的 NV12、YUYV），探测不到退回 `/dev/video0`。仅 getter，设置页不编辑 |
| `websocket` | Pad↔Aibox 客户端：`server_url`（空=整条链路不启动，设置页可改）+ 心跳/重连间隔（仅 JSON）。**本机是客户端，这条连接本身不监听端口**；它是旧协议，v1 之后由下面的 `pad_server` 取代（去留待接口方确认） |
| `pad_server` | **Pad 接口 v1 的入站服务端**（本机是 WS 服务端，Pad 连进来收报警）。`enabled` **默认 `false`**——这是一个明文零鉴权的入站口，局域网内任何设备都能连。`port` 默认 8899（v1 占位；与 `media_http_port` 8090 是两个口）；`media_base_url` 空=复用内嵌文件服务的自动地址。只能改 JSON、无 setter（改口要重启）；握手只放空 Origin 与 `android-app://*`（其余 403，判据 `originAllowed()` 一处）。落点 [src/biz/pad/](src/biz/pad/README.md)，键表见 [src/base/config/README.md](src/base/config/README.md)。**本轮只落 WS 报警面，`/api/` HTTP 面未实现** |
| `alarm_uplink` | 本机报警的**上行推送**（`enabled` **默认 `false`**——往外发数据必须人开）。两段式：报警当刻发 `alarm_info`（文字 + 媒体地址承诺），媒体写稳后再发 `media_begin`。`media_transport` 默认 `url`＝本机起内嵌 http 静态文件服务（**监听 `media_http_port`，默认 8090，明文零鉴权**）只在 JSON 里发地址；`ws_binary`＝字节走 WS 分块帧（已实测，代码保留、非默认）。只能改 JSON，设置页无控件；键表见 [src/base/config/README.md](src/base/config/README.md) |

另有一整段 **`gb28181`** 不经 `ConfigManager`，由 `src/biz/sip/gb28181_config.cpp` 直接读文件
（国标推流的平台参数与通道列表；无此段即不启用推流），但会被 `save()` 原样保留。明细见
[src/base/config/README.md](src/base/config/README.md)、[src/biz/sip/README.md](src/biz/sip/README.md)。

报警记录写入 `data/idge.db`（Qt SQL 检测库），抓拍图片保存在 `data/alarms/YYYYMMDD/` 目录。
打开 `recording.enabled` 后（出厂即开），同一目录还会多出报警前后共 5 秒、**带检测框**的视频片段：
`ch{N}_{类别}_{HHmmss_zzz}.h265`（Annex-B 裸流，用于上传/国标）与同名 `.mp4`（网页播放副本）。
片段由后台线程在报警后 2 秒落盘，路径回填到 `alarms.video_path`，详情对话框的"视频"行可见。
⚠ 开启 `alarm_uplink` 的 `url` 档（默认档）后，**`data/alarms/` 这棵树会被内嵌 http 服务整体对外可读**
（监听 `media_http_port`，无鉴权，只关目录索引）——该目录即暴露面，按天保留与 4GB 容量回收会删掉旧地址，
所以发给对端的地址是 best-effort。部署前确认网络隔离范围（见 `src/media/server/README.md`）。

## 4. 其他常用脚本

```bash
./build-linux.sh -t rk3588 -b Release   # 构建（见下文"构建"）
./video-preview.sh                       # MIPI CSI 摄像头预览调试
./dump_hang.sh                           # 卡死现场抓取（gdb 堆栈）
```

## 5. 页面操作速览

| 页面 | 入口 | 功能 |
|------|------|------|
| **扫码登录** | 启动后第一屏（无入口，未登录时盖住主界面） | 左品牌块 + 右侧竖排三钮：扫码登录（窗格打开即扫描，解出后点「确认登录」）/ 测试登录（读 `plan/construction_qr.json`）/ 退出程序 |
| 视频监控 | 顶部"视频监控" | 4 路实时画面 + FPS + 围栏叠加显示 |
| 数据看板 | 顶部"数据看板" | 报警统计 KPI、类别分布、通道在线状态（2 秒刷新，切走即停表） |
| 报警数据 | 顶部"报警数据" | 历史报警列表（含**负责人**列）、图片查看、确认/误报操作 |
| 人员点名 | 顶部"人员点名" | 人脸注册 / 现场点名 / 注销比对（摄像头抓拍） |
| 设备盘点 | 顶部"设备盘点" | 拍照 → YOLO 识别 → 盘点清单与明细 |
| 智能识别 | 顶部"智能识别" | 二维码实时扫描（ZBar）+ 设备铭牌文字识别（PP-OCRv4 两阶段） |
| 调试设置 | 顶部"调试设置" | 通道/模型/阈值/围栏配置（即改即存）+ 运行日志 |
| 退出登录 | 顶部"退出登录" | 停流并回到扫码登录页（进程不死）；**唯一的退出程序入口在登录页上** |

登录成功后，标题栏**右上角**常驻一颗「登录人 张三 · SG-2026-11」角标（未登录时整颗隐藏），
点开就是当前施工信息详情框（五项齐全）。这是界面上唯一能看出"现在系统记的是哪一班"的地方——
报警的归属在产生那一刻就定死了，换班若没点「退出登录」会继续记到上一班名下，
所以这个确认动作必须常驻、不能只留一行会滚走的日志。

顶部导航 6 项，按 `QToolButton` 文本分发（`frmMain::buttonClick()`），**`btnExit` 例外——按对象身份分发**
（改文本会让文本匹配静默失配）；页面控件指针不依赖 stackedWidget 索引，插页不会错位。
"智能识别"两项能力都受部署文件影响：缺 ZBar 库则二维码卡片置灰，缺 OCR 权重/可执行则文字卡片置灰，
页面本身照常可进。**登录页没有导航行可点**：`.ui` 里导航行与 `stackedWidget` 平级，盖不住，
所以门禁靠 `widgetTop->hide()`——留着就能点进智能识别页去抢扫码器正在用的那个摄像头节点。

**界面按板载 10 寸电容触摸屏设计**（实测 `DSI-1 1920x1200`、`goodix-ts`，≈226 PPI，1mm≈8.9 逻辑 px）：
导航钮是**纯文字无 icon**，全站按钮/输入件/表格行/滚动条按一组触屏命中区档位放大过，
"字号不动、只长高度"。档位的定义、来源与三条易踩口径（表格行高 CSS 管不到、皮肤层会被内联 QSS 打穿、
`getStyle()` 拆行会吃掉整条规则）记在 `src/ui/core/README.md` 的「触屏命中区设计令牌」。
已知阻断项：**没有虚拟键盘**，触屏上任何文本框（WS 地址、RTSP 路径、备注、改名、任务名）都输入不了；
**扫码登录页因此也没有手输入口**——现场二维码贴错了，唯一补救是重贴码，或走登录页的「测试登录」读文件。
多边形围栏需要右键或双击闭合，触屏上画不出来，只能在带鼠标的调试环境里画。

---

# 二、构建

## 系统要求

- **硬件**：Rockchip RK3588/RK3576/RK356x，建议 4GB+ 内存
- **操作系统**：Linux (aarch64)
- **依赖库**：见 `3rdparty/` 目录（版本明细见其 [README](3rdparty/README.md)）

## 1. 构建 rkmpp FFmpeg（板子上原生编译）

系统自带的 FFmpeg 4.x 不支持 rkmpp 硬解码，需从源码编译带 rkmpp 支持的 FFmpeg 6.1：

```bash
cd /tmp
git clone https://github.com/nyanmisaka/ffmpeg-rockchip.git -b 6.1 ffmpeg-rockchip
cd ffmpeg-rockchip
./configure \
    --enable-rkmpp --enable-libdrm --enable-version3 \
    --enable-shared --disable-static \
    --disable-doc --disable-debug \
    --prefix=/path/to/idge-main/3rdparty/ffmpeg-rkmpp
make -j$(nproc) && make install
```

## 2. 构建项目

```bash
./build-linux.sh -t rk3588 -b Release

# 可选参数：
# -t <target>   目标平台: rk356x / rk3588 / rk3576
# -b <type>     构建类型: Debug / Release
# -m            启用 Address Sanitizer (需要 Debug 模式)
# -r            禁用 RGA, 使用 CPU 缩放
# -d            启用 DMA32 (RGA2, 低于4G内存)
```

构建产物安装到 `install/rk3588_linux/`，可执行文件 RPATH 已设为 `$ORIGIN/../lib`。

## 3. 手动运行（开发调试）

```bash
export LD_LIBRARY_PATH=/usr/lib/aarch64-linux-gnu/mali:3rdparty/ffmpeg-rkmpp/lib:/usr/local/Qt-5.15.18/lib:$LD_LIBRARY_PATH
./build/build_rk3588_linux/idge
```

> 一般**不需要**手工 export：构建目录的 `idge` 已带绝对 RPATH。上面这行只在
> RPATH 里的 Qt 路径（本机是 `/usr/local/Qt-5.15.18/lib`，实际装在哪看 `ldd`）
> 与你的环境不符、需要覆盖时才用。注意直接在源码根执行本命令时，cwd 就是源码根，
> 读写的是 `./data/`；经 `run.sh` 启动则读写 `build/build_rk3588_linux/data/`。

### 关键环境变量

| 变量 | 值 | 说明 |
|------|------|------|
| `DISPLAY` | `:0` | X11 显示服务器 |
| `LD_LIBRARY_PATH` | 见上 | 动态库搜索路径 |
| `XDG_RUNTIME_DIR` | `/run/user/$(id -u)` | 用户运行时目录 |
| `IDGE_WORKSPACE` | 业务资源根目录 | 点名/盘点/智能识别模型与数据（缺省=当前目录） |
| `IDGE_PERF` | 任意非空值 | 打开性能探针日志（`paintGL` 间隔、面板刷新耗时、锁等待），板端定位卡顿时用；默认关闭且开销仅是一次 `QElapsedTimer::start()` |
| `EGL_NO_X11` | 构建时全局定义 | 强制 EGL 走 DRM 后端（CMake 已处理） |

### 库路径说明

| 库 | 路径 | 说明 |
|----|------|------|
| Mali EGL/GLES | `/usr/lib/aarch64-linux-gnu/mali/` | GPU 渲染驱动（系统安装） |
| ffmpeg-rkmpp | `3rdparty/ffmpeg-rkmpp/lib/` | 带 rkmpp 支持的 FFmpeg 6.1（板子原生编译） |
| Qt5 | `/usr/local/Qt-5.15.18/lib/` | Qt5 运行库（板子安装） |
| librga | `/usr/lib/` | RGA 硬件加速库（系统自带） |
| libmk_api | `3rdparty/zlmediakit/aarch64/` | 报警上行 url 档的内嵌 http 服务（进程级单例，`src/media/server/` 唯一封装点） |
| OpenSSL 1.1 | `3rdparty/openssl1.1.0/lib/` | `libmk_api.so` 的 NEEDED（`libssl.so.1.1`/`libcrypto.so.1.1`）；与系统 OpenSSL 3 并存，**仅供 ZLM 链接**，Qt 的 TLS 仍走系统 3（故 `wss://` 不可用，见 §八 第 11 条）。本工程链了 `--disable-new-dtags` → DT_RPATH 可传递，`run.sh` 不需要为此加 `LD_LIBRARY_PATH` |

### 网络端口口径（部署/防火墙要看的就这四条）

| 方向 | 端口 | 何时存在 | 说明 |
|------|------|----------|------|
| 出站 | 对端的 `websocket.server_url` | 配了地址才连（空=整条链路不启动） | 本机是 **WS 客户端**，不监听这条；**只填 `ws://`**，`wss://` 本机连不上 |
| **入站 TCP** | `alarm_uplink.media_http_port`，默认 **8090** | 仅 `alarm_uplink.enabled=true` 且 `media_transport="url"`（后者是默认档） | 内嵌 http 静态文件服务，根 = `data/alarms` 绝对路径，**只读、明文、零鉴权**（只关了目录索引）。报警媒体地址的交付面；想限来源网段可配库内的 `http.allow_ip_range`（默认未设＝不限）。详见 `src/media/server/README.md` |
| **入站 TCP** | `pad_server.port`，默认 **8899** | 仅 `pad_server.enabled=true`（**出厂 `false`**） | Pad 接口 v1 的 WS 服务端，`listen(QHostAddress::Any)`，**明文、零鉴权**（§2.3 规定不鉴权）。唯一的门是 Origin 白名单：空 Origin 与 `android-app://*` 放行，带 `http(s)://` Origin 的握手回 **403**——判据在 `pad_ws_server.cpp` 的 `originAllowed()` 一处，放行别的来源改那里并回来更新本行。口径见 `src/biz/pad/README.md` |
| 出入站 UDP | `gb28181.platform.localSipPort`（默认 5060）本机 `bind` | 仅 `gb28181` 段能凑出 platform + streams | SIP 注册/心跳/MANSCDP 收发（本机也收平台报文，但它是 UDP、不是 TCP 监听口）；RTP 推流另按 SDP 协商走 TCP/UDP。**当前 `platform.transport` 不影响发包，固定 UDP** |

`ss -ltnp | grep -E '8090|8899'` 为空 + `/proc/<pid>/task` 里没有 ZLM 线程，是"出厂态两个入站口都没开"的两个证据
（别看日志行数）。

---

# 三、主要特性

- **全链路零拷贝**：从 FFmpeg 解码到 EGL 渲染，全程通过 DMA-BUF fd 传递，无 CPU 内存拷贝
- **3 NPU 核心并行推理**：充分利用 RK3588 的 3 个 NPU 核心，实现高吞吐量检测
- **RGA 硬件加速预处理**：色彩转换和图像缩放由 RGA 引擎完成，释放 CPU
- **报警视频硬件编码**：带检测框的 RGBA → RGA → `hevc_rkmpp` 零拷贝硬编 + 环形缓冲，报警时切出"前 3 秒 + 后 2 秒"裸流片段 + 同名 `.mp4` 网页副本（CPU 软编 x264 已彻底移出链路）
- **电子围栏**：多边形/矩形围栏绘制，**每个形状自带角色**——禁区（不能进）与岗区域（只能在里面待着）
  可在同一通道混用；围栏**独立于级联**——
  复用该通道已有模型的检测结果做几何判定，触发类别是 `geofence.alarmClass` 单值（默认 `person`）；
  报警按**进入沿**触发（闯禁区报一条、站着不动不报、离开再回来算新的一条；
  人在所有岗区域之外报「离岗」，回岗待一会儿再出来才算新的一条），
  禁区命中优先于离岗判定、一人一帧一条，
  `geofence.throttleSeconds`（默认 10 秒）只作每通道每角色每类别的节流下限
- **扫码登录（门禁）**：启动后先进登录页，用本机摄像头扫现场张贴的施工二维码（码内容即施工信息 JSON：
  施工id / 负责人姓名·电话·工号 / 施工工作内容）。**未登录不推流、不检测、不出报警**；
  必填只有 `construction_id` 与 `worker_name`，缺字段直接拒绝并点出缺哪个。每条报警都带上这份信息
  （落库两列、详情框逐键显示、上行 `alarm_info.data.construction` 嵌套子对象）。
  导航的「退出」改成「退出登录」（回登录页，进程不死），登录页上有唯一的「退出程序」。
  登录期间标题栏右上角常驻「登录人 + 施工id」角标，点开看全量五项（`ConstructionDetailDialog`）。
  ⚠ 门挡的是"检测与报警产生"，**不是整个进程的网络出口**：GB28181 与 WsClient 仍按配置在启动时起来。
- **报警管理**：限流去重、抓拍落盘、SQLite 持久化、GUI 悬浮提示与历史查询
- **人员点名**：SCRFD 人脸检测 + 特征比对（进程内 RKNN 常驻推理），注册 / 点名 / 注销三阶段，余弦相似度 + 贪心一对一匹配
- **设备盘点**：复用 YOLO11 RKNN 模型做设备识别与盘点任务管理
- **智能识别**：二维码扫描（ZBar，模态窗格**打开即实时扫描**，解出后显示码内内容、点确认才关窗格）+ 设备铭牌文字识别（PP-OCRv4 det/rec 两阶段，外部 demo 经 QProcess 调用）。
  扫码器全程序**只有一份**（`uiwidgets::QrScanDialog`），登录页与智能识别页共用——`smartRecognition.camera`
  那个设备节点不能被开两次
- **GB/T 28181 国标推流**：SIP 注册/心跳/目录 + MPEG-PS 复用 + RTP/PT96 直通 H.264 码流，按需启用（无配置段即静默跳过）
- **Pad↔Aibox 对接**：WebSocket 客户端接收远端报警推送并回 ACK，应用层心跳 + 断线自动重连（未配置地址即整条链路缺席）
- **报警上行（两段式）**：`alarm_uplink.enabled` 打开后订阅本机 `AlarmManager`，报警当刻先推 `alarm_info`（文字 + 截图/片段地址承诺），
  文件写稳再推 `media_begin`——**默认走 `url` 档**：进程内挂一个 ZLMediaKit http 静态文件服务（根锁 `data/alarms`、只读、无鉴权），
  JSON 里只发地址、字节一律不进 JSON；`ws_binary` 档（同一份字节走 WS 二进制分块帧）已板端实测、代码保留可切回。
  可靠性 = 待确认内存队列 + 写缓冲水位闸门 + 未发出去帧断线续传（**无落库 outbox、进程重启不补历史**）；
  对端确认 `uplink_ack` 与指数退避重投已实现但默认关闭（`expect_ack=false`，对端当前不回这个类型）；
  媒体拿不到时发 `media_failed` 撤承诺，绝不让对端攥着一条永不兑现的 `planned`
- **Qt5 GUI**：blacksoft 换肤框架，无边框窗体，多页面导航
- **多路视频支持**：支持 4 路视频同时监控
- **模型灵活配置**：支持 YOLO11 nano/small/medium 不同大小模型与级联多模型

# 四、技术架构

## 核心技术栈

| 技术 | 用途 |
|------|------|
| C++17 | 主要开发语言 |
| Qt5 | GUI 框架 |
| RKNN Runtime (rknpu2) | Rockchip NPU 推理引擎 |
| FFmpeg (rkmpp) | 视频硬件解码 (h264/hevc_rkmpp) 与硬件编码 (hevc/h264_rkmpp) |
| RGA | 硬件 2D 图像加速（色彩转换 + 缩放） |
| OpenGL ES 2.0 / EGL | GPU 零拷贝视频渲染 |
| SQLite3 | 检测库 (idge.db) + 业务库 (roll_call.db) |
| Eigen | 人脸特征批量相似度矩阵计算 |
| CMake 3.10+ | 构建系统 |

## 视频处理全流程

### 输入格式
视频源可以是本地 `.mp4` 文件或 RTSP 摄像头流，通过 FFmpeg 的 `avformat` 打开，由 Rockchip MPP
后端（VPU）硬解输出 DRM_PRIME(NV12) DMA-BUF。
⚠️ **硬解器名目前写死优先 `h264_rkmpp`**（不按流的 `codec_id` 选路），所以 **H.264 源是唯一已验证路径**；
4 路摄像头请统一投 H.264 基线，H.265 源要等"按 codec_id 选硬解器"改造后才能用（roadmap P6 首项）。

### 格式转换链路
```
H.264 编码流
    ↓  FFmpeg h264_rkmpp 硬解码（VPU 硬件解码）
NV12 DMA-BUF fd（YUV 420 半平面，GPU 可直接访问）
    ↓  RGA 硬件色彩转换（fd → fd，零拷贝）
RGBA8888 DMA-BUF fd（RGBA 4 通道，OpenGL 可直接导入）
    ↓  CPU 侧画框 + 标签（draw_rectangle / draw_text，mmap 直写像素）
RGBA8888 DMA-BUF fd（带检测框的最终画面）
    ↓  dup(fd) 传递给渲染线程
    ├──→ EGLImage 导入 DMA-BUF（零拷贝）→ OpenGL ES 纹理绘制 → 屏幕显示
    └──→ RGA 缩放到 640×640 RGB → RKNN NPU 3 核并行推理（YOLO11）
             ↓
         NMS 后处理 → 检测结果队列 → 画框叠加到下一帧
             ↓
         电子围栏判定（脚点坐标映射到 widget 空间，按形状角色出闯入/离岗）→ 报警限流 → 抓拍落盘 + 写 idge.db

带检测框的 RGBA8888 DMA-BUF fd
    ↓  RGA rgba_to_nv12（fd → fd，零拷贝）+ DRM_PRIME 池帧
    ↓  hevc_rkmpp 硬编码（VPU 硬件编码，CBR / GOP 1s / bframes=0）
Annex-B H.265（或 h264_rkmpp → H.264）AVPacket —— **同一份包两路消费，不重复编码**
    ├──→ 环形缓冲区（默认 5s，drop-front）
    │        ↓  报警 +2s 到点后由 ClipWriter 后台线程取"前 3s + 后 2s"窗口（起点对齐 IDR）
    │    .h265 裸流（上传/国标）+ 同名 .mp4（网页副本）→ 路径回填 alarms.video_path
    └──→ 上传服务（P4，服务器 URL 预留接口）
```

### 各阶段详解

| 阶段 | 输入格式 | 输出格式 | 执行硬件 | 关键代码 |
|------|----------|----------|----------|----------|
| 解码 | H.264 码流 | NV12 DMA-BUF | VPU (MPP) | `src/media/reader/ffmpeg_video_decoder.cpp` |
| 色彩转换 | NV12 DMA-BUF | RGBA DMA-BUF | RGA 硬件 | `src/media/rga/rga_converter.h` |
| 推理预处理 | RGBA DMA-BUF | 640×640 RGB DMA-BUF | RGA 硬件 | `RgaConverter::rgba_to_rgb_resize()` |
| NPU 推理 | 640×640 RGB | 检测结果 (80类) | NPU ×3 | `src/ai/yolo11/yolo11_model.hpp` |
| 画框叠加 | RGBA DMA-BUF + 检测结果 | 带框的 RGBA DMA-BUF | CPU (mmap) | `src/base/imaging/draw_utils.*` |
| 渲染 | RGBA DMA-BUF fd | 屏幕像素 | GPU (Mali) | `src/ui/widgets/EglImageRenderer.h` |
| 围栏/报警 | 检测结果 | 报警记录 + 抓拍图 | CPU | `src/biz/geofence/`、`src/biz/alarm/` |
| 反向色彩转换 | RGBA DMA-BUF (带框) | NV12 DMA-BUF | RGA 硬件 | `RgaConverter::rgba_to_nv12()` |
| 硬编码 | NV12 DMA-BUF | H.265/H.264 Annex-B 包 | VPU (MPP) | `src/media/reader/video_recorder.cpp` |
| 片段封装 | 环形缓冲窗口包 | `.h265` 裸流 + `.mp4` 副本 | CPU (仅 memcpy/remux) | `VideoRecorder::dumpAnnexB/dumpMp4`、`src/media/reader/` ClipWriter |

## 业务模块调用链

```
人员点名：摄像头预览(src/media/reader/camera_preview_decoder) → 抓拍 JPEG
    → SCRFD 检测 + 五点仿射对齐 + 512 维特征提取(进程内常驻
      src/ai/recognition/in_process_face_recognizer)
    → 注册/比对/注销(src/biz/service/roll_call_service)
    → 余弦相似度(归一化点积) + 贪心一对一匹配 → roll_call.db

设备盘点：拍照 → 进程内 YOLO11 识别(src/ai/yolo11，权重=model/library/yolo11n-coco)
    → 结果去重汇总 → src/biz/service/equipment_inventory_service → roll_call.db 任务/设备表

智能识别：对话框打开时新建 FFmpegVideoDecoder(InferenceMode::PreviewOnly——只解码取帧，
    不 buildCascadeTasks，一个 YOLO/RKNN 模型都不加载) 拉 smartRecognition.camera(缺省自动探测摄像头节点)
    → 二维码：captureCurrentFramePtr 取帧 → RGBA→灰度 → ZBar 扫描 → 三帧内容一致才判定
      （模态对话框期间才有工作线程，关窗即释放）
    → 铭牌 OCR：整幅拍照图 → 3rdparty/ppocrv4/bin/rknn_ppocr_det_demo 出文本框
      → 按行聚成选区供用户点选 → 裁图 → rknn_ppocr_system_demo 出行文本

国标推流：main() 判据(Gb28181Config::load 能否凑出 platform + streams) → 命中才 start()
    → Gb28181Manager 单 UDP socket 事件循环（REGISTER/Digest、Keepalive、MANSCDP 查询）
    → 平台 INVITE → 每路一个 Gb28181MediaSource 独立取流(不共用主链路帧)
      源本身是 H.264 → h264_mp4toannexb 直通；否则 rkmpp 硬解(退软解) + h264_rkmpp 重编(退 libx264)
    → PsMuxer(PS 打包，等齐 SPS+PPS+IDR 才出帧) → RtpSender(PT96) → 平台

Pad↔Aibox 入站：frmMain 构造 → WsClient::start() → QWebSocket 连 websocket.server_url
    → 收 type=alarm → frmMain::onWsAlarmReceived → 调试页日志 + sendAck（不入 idge.db）
    → 应用层 ping 心跳 + 断开后定时重连

报警上行（出站，alarm_uplink.enabled 才挂）：
    frmMain 装配 → url 档先起 media::HttpFileService(root=data/alarms 绝对路径, media_http_port)
      → AlarmUplink::setMediaBaseUrl() → attach() 订阅 AlarmManager::alarmGenerated
    → 报警当刻：AlarmUplinkEngine 组装 alarm_info（截图/片段以 planned 地址形式申报）入队 → WsClient 发文本帧
    → 媒体写稳：MediaPump 后台线程等 size 稳定 → 读全量算 sha256 → 回 GUI 线程
      → url 档发 media_begin{transfer:"http", url, total_bytes, sha256}（一个字节都不进 WS）
      → ws_binary 档发 media_begin + binary 分块 × N + media_end
    → 落空则发 media_failed 撤承诺；队列按水位闸门逐帧外推，发送失败保持顺序原地重试
```

---

# 五、目录结构与模块文档索引

**每个源码目录都有 README.md 详述功能、数据流、使用方法与注意事项**，下表为速查索引。

### 功能域总览（依赖方向 base ← media ← ai ← biz ← ui）

| 域 | 模块 |
|------|------|
| `src/base/` 基础设施 | buffer、queue、config、imaging、runtime_paths.h、perf_probe.h |
| `src/media/` 视频管线 | reader、rga、server(报警上行的内嵌 http 静态文件服务) |
| `src/ai/` 推理与模型应用 | yolo11、model_repo、recognition、task |
| `src/biz/` 业务域 | alarm、geofence、service、db(报警库+业务库)、sip(GB28181)、ws(Pad↔Aibox 客户端 + 报警上行引擎) |
| `src/ui/` 界面层 | form、widgets(原 src/ui)、core(主题/Qt 助手 + qss 皮肤与资源) |

### 模块速查

| 目录 | 职责 | 文档 |
|------|------|------|
| `src/main.cpp` | 应用入口（CLI/GUI 双模式、EGL 初始化、data/ 旧布局迁移） | — |
| `src/base/runtime_paths.h` | 运行时数据路径唯一事实源（data/ = **当前工作目录**相对，非源码根） | [README](src/base/README.md) |
| `src/ui/form/` | 主窗口与全部页面/对话框（视频、看板、报警、点名、盘点、智能识别、设置） | [README](src/ui/form/README.md) |
| `src/ui/widgets/` | EGL/OpenGL ES 零拷贝渲染、视频控件、看板、报警列表 | [README](src/ui/widgets/README.md) |
| `src/ui/core/` | 主题/Qt 助手/图标字体/启动钩子 + blacksoft 皮肤与字体、图片、翻译资源 | [README](src/ui/core/README.md) |
| `src/media/reader/` | FFmpeg 解码线程、硬编环形录像(VideoRecorder/ClipWriter)、摄像头预览解码、SCRFD 人脸检测 | [README](src/media/reader/README.md) |
| `src/media/rga/` | RGA 硬件加速封装 | [README](src/media/rga/README.md) |
| `src/media/server/` | 报警上行的内嵌 http 静态文件服务（ZLMediaKit `mk_*` 的唯一封装点，进程级单例、不可重入） | [README](src/media/server/README.md) |
| `src/ai/yolo11/` | YOLO11 RKNN 推理核心、前后处理 | [README](src/ai/yolo11/README.md) |
| `src/ai/model_repo/` | 模型库注册表（library 扫描/元数据/级联槽位解析） | [README](src/ai/model_repo/README.md) |
| `src/ai/task/` | 检测任务体系（PPE 任务 + 任务基类/配置/结果数据结构） | [README](src/ai/task/README.md) |
| `src/ai/recognition/` | 人脸识别：进程内 `InProcessFaceRecognizer`（点名生产链）+ 外部 exe 桥接 `FaceRecognitionWrapper`（对照/CLI） | [README](src/ai/recognition/README.md) |
| `src/biz/service/` | 人员点名 / 设备盘点业务服务 | [README](src/biz/service/README.md) |
| `src/biz/alarm/` | 报警管理（限流、去重、抓拍、SQLite 持久化） | [README](src/biz/alarm/README.md) |
| `src/biz/geofence/` | 电子围栏（形状、角色、判定、绘制 overlay） | [README](src/biz/geofence/README.md) |
| `src/biz/db/` | 数据持久层：报警库 idge.db（QtSQL DAO）+ 业务库 roll_call.db（sqlite3 原生） | [README](src/biz/db/README.md) |
| `src/biz/sip/` | GB/T 28181 设备侧：SIP 注册心跳、MANSCDP、PS 复用、RTP 推流 | [README](src/biz/sip/README.md) |
| `src/biz/ws/` | Pad↔Aibox WebSocket 客户端（入站报警 + 心跳重连，预留 get_streams/set_fence）**与本机报警上行**：报文组装、内存队列、水位闸门、ack/退避、媒体读盘泵（`alarm_uplink*`/`media_pump`）。**旧协议**，v1 之后待整块退役（报告冲突 B1） | — |
| `src/biz/pad/` | **Pad 接口 v1 的服务端**（本机是被连的一方）：`QWebSocketServer` 收发 `hello_ack`/`pong`/`alarm`/`alarm_update`/`sync_result`/`error`，外加 `AlarmRecord`→v1 JSON 的唯一映射层。与 `src/biz/ws/` 方向相反，两套不能并存 | [README](src/biz/pad/README.md) |
| `src/base/config/` | ConfigManager 单例 + data/config.json 解析 | [README](src/base/config/README.md) |
| `src/base/buffer/` | DMA-BUF 分配与帧缓冲池 | [README](src/base/buffer/README.md) |
| `src/base/queue/` | 阻塞队列 / 优先级队列 / 最新帧槽 | [README](src/base/queue/README.md) |
| `src/base/imaging/` | 图像读写/格式转换/letterbox、软件画框、Qt 安全解码、文件读写原语 | [README](src/base/imaging/README.md) |
| `src/base/sysinfo/` | 设备运行指标采样（CPU/内存/温度/电池/NPU/磁盘/uptime/系统版本）——看板与 Pad `/api/device/status` 的同一份口径 | [README](src/base/sysinfo/README.md) |
| `tools/face_recognition/` | 人脸识别命令行工具（CLI 验证/点名对照回归基准，产物输出 model/face/） | [README](tools/face_recognition/README.md) |
| `model/` | 模型库 `library/<id>/`（yolo11n/s/m-coco、ppe、ocr 等，一个模型一个目录）+ `train/`（训练侧 .pt/yaml/校准清单，程序不读） | [README](model/README.md) |
| `data/` | 运行时数据（config/idge.db/alarms/backups/roll_call_data，内容不入库） | — |
| `python/` | 模型转换与验证脚本：`.pt → onnx → rknn`（`pt2rknn.sh`）、ONNX→RKNN（`convert.py`）、推理/mAP 验证（`yolo11.py`） | [README](python/README.md) |
| `tests/` | 测试程序（test_database 数据库层；test_equipment 设备盘点、test_rollcall 人员点名全链离线自测；test_latest_frame_slot 帧广播槽跳帧/生命周期；test_video_sources 视频流注册表与通道引用解析；test_stream_params 流参数板端实测；test_gb28181_source_ref 国标流引用注册表；test_pad_* Pad v1 三连，均随主构建生成） | [README](tests/README.md) |
| `docs/` | 技术知识库 | [README](docs/README.md) |
| `res/` | Qt 资源（qrc：图片/字体/GL 着色器/音效） | [README](res/README.md) |
| `sounds/` | 提示音文件 | [README](sounds/README.md) |
| `3rdparty/` | 第三方库；`nlohmann/`（JSON 单头）、`zbar/`（二维码）与 `ppocrv4/`（OCR demo）的**二进制产物不入库**，新克隆需按其 README 手工还原 | [README](3rdparty/README.md) |

运行时数据统一在 **当前工作目录下**的 `data/`（内容已 gitignore，不入库）：`data/config.json`、`data/idge.db*`、`data/alarms/`（按日抓拍）、`data/backups/`、`data/roll_call_data/`。因而是"每个启动位置一套"——`run.sh` 启动的那份在 `build/build_rk3588_linux/data/`。旧布局（散落在根目录）启动时由 `RuntimePaths::migrateLegacy()` 自动迁入。构建目录 `build*/`、`install/` 同样不入库。

### 根目录文件

```
├── CMakeLists.txt              # 主构建配置
├── build-linux.sh              # 构建脚本（-t rk3588 -b Release）
├── run.sh                      # 一键启动（自动设置库路径）
├── video-preview.sh            # MIPI CSI 摄像头预览脚本
├── dump_hang.sh                # 卡死堆栈抓取
├── assets/                     # 随仓库分发的手工测试素材（test/1.mp4、192.mp4 等，构建目录软链过去）
├── data/                       # 运行时数据根（config.json/idge.db/alarms/backups/roll_call_data）
└── config_example.json         # 配置模板
```

---

# 六、模型信息

- **模型格式**：RKNN (Rockchip Neural Network)，转换方法见 [python/README.md](python/README.md)
- **检测模型**：YOLO11 nano/small/medium（COCO 80 类），输入 640×640（LetterBox 预处理）
- **人脸模型**：SCRFD 检测 + 特征识别，放置于 `$IDGE_WORKSPACE/model/face/`
- **OCR 模型**：PP-OCRv4 det/rec 两件套（`model/library/ppocrv4-ocr/` 的 `det.rknn`/`rec.rknn`/`labels.txt`），
  由 `3rdparty/ppocrv4/bin/` 的两个 demo 进程加载，主程序只传图路径、解析 stdout
- **量化支持**：uint8 / int8 / float32
- **NPU 核心**：可指定 RK3588 的 3 个 NPU 核心（`detect.threads` 配置）

# 七、第三方库版本

明细见 [3rdparty/README.md](3rdparty/README.md)。摘要：FFmpeg(rkmpp) 6.1.1、MPP 1.3.10、RKNN Runtime、librga 1.10.0、OpenCV、Qt5 5.15.18、jsoncpp 1.9.7、libjpeg-turbo、libyuv、libsndfile、OpenSSL（**系统 3.x 给 Qt / 入库 1.1.0 给 ZLMediaKit，两份并存**）、ZeroMQ、FFTW、stb、OpenCL stub、DRM 分配头文件、sqlite3、ZBar（二维码，头文件与 `zbar.pc` 入库、`libzbar.so` 不入库）、PP-OCRv4 RKNN demo（第三方，见 `3rdparty/ppocrv4/`）、ZLMediaKit（入库的是预编译 `3rdparty/zlmediakit/aarch64/libmk_api.so` + 头文件，**当前只用于报警上行的内嵌 http 静态文件服务**，流媒体能力未编译未接入）。

# 八、已知问题与技术债

> 需求缺口与实施顺序见 `plan/requirements.md` / `plan/roadmap.md`；
> **两者的状态结论已被 `plan/需求完成度审计_2026-10-30.md` 按代码复核过一轮**（三处被推翻、两处被过度记载），
> 完成度对照、假开关名单与分四轮的下一轮计划以那篇为准。
> （`plan/` 整目录在 `.gitignore` 里——里面是甲方需求原件与本地工作笔记，不随克隆分发。）
> 本节只列**代码级**仍开的问题，均已（或应在）源码注释中警示，细节另见各目录 README 的"注意事项"。

1. `DmaBufferPool::acquire` 返回裸指针，调用方须在池销毁前回借出；池析构会强制回收未归还项
   并打印警告（悬垂风险仍在，靠约定规避）。`DmaFrameBuffer` 浅拷贝双释放已用 `= delete` 堵死
2. **JSON 不能加注释**：`QJsonDocument::fromJson` 遇到 `//` 直接返回 null，而解析失败走的是
   "warning + 全默认值"分支——写注释不是启动报错，而是**静默丢掉整份现场配置**。且 `save()`
   是整文件重写、按键名字母序输出，任何排版都留不住。要放说明只能用数据键
   （`config_example.json` 里的 `_readme` 字符串数组即可原样往返）
3. `src/ui/form/photo_selection_dialog.h`、`src/ui/form/video_decoder.*`、
   `src/ai/task/base_task.*` 都在 GLOB 里被编译但无实例化点（清单见 roadmap §六；
   其余零调用文件已分两批删除——第一批 helmet_task/task_pool/frame_queue/ModelPool/ThreadPool/xml_utils，
   第二批（结构调整时顺带）core_helper 的 appdata/customstyle/delegate/base64helper 四对 + base/utils 的 logging/path_utils）
4. **阈值只到"每模型"这一层**：`conf_threshold` 的键是**库 id**，同一个模型挂在几路就几路共用
   一个值（设置页"被引用通道"列显示会 fan 到哪几路）；要按通道分得把 `channels[].models[]`
   从字符串数组变成对象 + schema 迁移，没做。`nms_threshold` **只有全局一档**，也没有每模型覆盖
5. `YOLO11Model::output_mems[9]` 固定长度，换多于 9 个输出头的模型（分割/姿态）会越界
6. **GB28181 MANSCDP 编码声明与实际不符**：XML 头写 `encoding="GB2312"`，实际发出的是 UTF-8 字节
   且 `Content-Length` 也按 UTF-8 长度算（`gb28181_manager.cpp` 心跳/应答处）。宽容平台无感，
   严格平台在中文 `DeviceName` 上会解析错乱。真要对接，改一处即需全模块统一口径
7. **GB28181 平台地址写错只表现为"不启用"**：`serverHost`/`advertiseHost`/SDP `c=` 必须是 IPv4 字面量，
   `inet_pton` 失败即静默 `return` 且无日志；`platform.transport` 是假开关（值不影响发包）。
   但**流被丢弃是有日志的**：`GB28181 stream dropped: 第 N 条流…`（缺 `deviceCode`、缺 `source_id`、
   `source_id` 在 `video.sources[]` 里查不到），先 `grep dropped` 再怀疑网络
8. **本机摄像头无互斥**：`smartRecognition.camera`（二维码实时扫描 / OCR 拍照 + 人员点名/设备盘点预览）
   与 `video.sources[]` 两条路径各自 `avformat_open_input`，同一设备被两路同时打开时后开的那路失败，
   报错只落在各自日志里。
   注册表解决的是"地址只写一次"，**不是**"设备只开一次"：两路通道引用同一个 `type=camera` 条目、
   或**某个通道与某条国标流引用同一颗摄像头**（`gb28181.streams[].source_id` 现在指向同一张表），
   平台点流时那次 `open` 会拿到 `Device or resource busy`
9. **扫码无去重/冷却**：窗格打开即扫，每 250ms 解一帧，解出即冻结画面等用户点「确认」。
    同一颗码举在镜头前时点「重新扫描」会立刻再解出同一内容（"重扫就是要它"，不是 bug）；
    跨会话也不去重，换一班次的码照样立刻解出
10. **通道配置改完就生效，但"生效"分两种**：报警类别（`video.models[<id>].alarm_class`）与
    每模型阈值（`video.models[<id>].conf_threshold`）是**在线换**
    ——解码线程每帧在 `slotParamMtx_` 下拷一份 `slotParams_`（报警类别 + conf + nms 同一个快照），
    既构造报警上下文、也填进这一帧的 `TaskData`，下一帧起就按新值走，这一路不重连；
    通道 `models` 与通道 `enabled` 则**重开该路**：停解码 → `releaseTasks()` → 按新配置重建任务集合。
    重开的代价要认：该路重新拉流（本地文件回到开头、RTSP/国标是重连），GUI 正常停顿 <1 秒，
    最坏被驱动卡住时按 `waitForStop()` 的有界等待走。出口只有 `frmMain::applyChannelConfigNow()` 一个。
    `PpeTask::reloadModel()`（同数量原地换模型）仍未接线，留给 roadmap P1-2。
    另：模型库的「测试选中」按钮会短时独占一次 NPU 上下文（GUI 阻塞秒级），与热更新无关。
    **设置页各改动的生效时机**（"改了没反应"先对这段口径）：
    即时生效——围栏形状与角色（`FenceManager` 内存态，解码线程每帧现取；角色只能在画之前选，
    画完要改只能删了重画）、围栏类别、围栏报警最小间隔、
    WebSocket 地址（重连）、通道备注/模型备注、模型 `alarm_class`、**模型 `conf_threshold` 与
    全局 `detect.conf_threshold`/`nms_threshold`**（阈值随帧走：解码器每帧取一次槽位参数快照
    塞进 `TaskData`，下一帧的 `detect()` 就用新值，约 1 秒内可见；前提是该路槽位数没变，
    变了就退回重开那一路）、模型库导入与删除
    （盘点服务 `reloadEquipmentService()` 热更）；
    重开该路——通道 `models`、通道 `enabled`、移除模型条目（槽位数变了，只能重建任务集合）、
    **每路的「选流」下拉、「浏览」与「清空」**（改的是这一路开哪条流，配置与画面必须同时走，
    不存在"改了要重启才生效"的空档）；「视频流管理…」确定后按 `touchedSourceIds()`
    重开**引用了被改动流**的那几路（删除一条流会连带清掉引用它的通道的 `source_id`，
    那几路同样在重开之列，判据是对话框前后 `channelSourceId()` 变了）；
    **国标推流**也从这张表取流，改地址**下一次 INVITE 就生效**（`Gb28181MediaSource::run()` 每次开流现解析），
    但国标段自己的身份键与 `source_id` 属"只落盘"——`Gb28181Config::load()` 只在启动时读一次，改了要重启；
    只落盘——视频流的**备注**（touched 判据只看影响解码的十项参数，备注改了不用重开）、
    以及直接手改 `data/config.json` 的任何键（运行中的程序不感知文件，要重启或在页面上动一次该通道配置）；
    `geofence.enabled` 设置页里没有，手改 config 后要么重启、要么在页面上动一次「围栏类别」触发 `loadFromConfig()` 顺带重载；
    `geofence.mode` 已退役——手改它不会改变任何判定，形状的角色只认 `channels/*/shapes[]` 里的 `role` 键；
    `detect.class_num`/`detect.threads` 是假开关，程序根本不读（见 `src/base/config/README.md`）。
    热更新的离线自测：`./build/build_rk3588_linux/test_channel_reload`（必须在 build 目录下跑，
    它驱动真解码器 + 真推理任务做 4 轮"停→释放→重建"，并检查线程数与 RSS 不涨）。
11. **WS 走明文 `ws://`，`wss://` 本机不可用**：`/opt/qt5.12.8` 的 QtNetwork 按 OpenSSL 3 编译但符号表仍找
    OpenSSL 1.1 的老名字（`SSL_get_peer_certificate`/`EVP_PKEY_base_id` dlsym 失败），TLS 握手路径等于空指针；
    当前决定不做适配，传输加密靠网络隔离兜（`WsClient::onSslErrors` 因此是死代码）。
    `WsClient::sendJson()` 本身**不排队**：未连接时直接丢弃并记 warning，所以入站报警的回执 `ack`
    在断线期间是真的丢了。**报警上行不受这条影响**——`AlarmUplinkEngine` 有自己的待确认队列，
    没发出去的帧留在队首、连上后按原顺序续发。
13. **开启报警上行的 `url` 档，本机就多监听一个明文端口**：默认 `media_http_port=8090`，
    根目录锁在 `data/alarms`、**只读且零鉴权**（只关了目录索引，知道文件名就能取）。
    这是"报警媒体交付方式"换来的代价，出厂 `alarm_uplink.enabled=false` 就是为了不默认开它；
    启用前必须确认网络隔离范围。三条硬约束：①`data/alarms` 里的文件名等于可下载的地址，
    该目录不得放任何非报警文件；②地址有效期无保证——按天保留 + 4GB 容量回收会删掉**当天**目录，
    对端必须"收到即取回"，404 是正常终态；③端口/开关变更**必须重启进程**，
    ZLM 的 `mk_env_init` 是进程级一次、`stop` 后不能再 `start`，所以设置页没有"改了重开服务"这条路
    （`src/media/server/README.md`）
12. **NPU 负载按"各通道模型数之和"计**：4 路 × 每路 3 个模型 = 12 个推理任务打在 3 个核上，
    配置不做硬拦截；任务数超过 6 时启动日志打软告警，核掩码按进程级轮转分配（跨通道共享一份
    模型上下文服务多路，仍是未做的优化）
14. **打开 `pad_server.enabled` 是本机第一次"入站监听"**：前面所有链路都是本机主动连出去
    （WS 客户端、GB28181 注册、报警上行），只有这一条是别人连进来。默认口 8899、明文 ws、
    **零鉴权、无 Token**（v1 §2.3 当前就不要求 Token）——局域网内任何能连到本机的设备都能
    收到实时报警、拉到历史报警快照与证据地址。所以出厂 `enabled=false`，与 `alarm_uplink`
    同一条"必须人来打开"的口径；启用前先确认网段隔离范围。唯一的门是**来源检查**：
    `originAllowed()` 放行空 Origin（原生 App 不发）与 `android-app://*`，带 `http(s)://` Origin
    的握手回 403——它挡的是"网页 JS 可连"，**不挡**裸 socket，所以这不等于访问控制。
    三条配套事实：
    ①`QWebSocketServer` 独占一只监听器，v1 §2.1 想让 WS 与 `/api/` 同占 8899 在这套栈上做不到；
    ②证据字节仍由 `media_http_port`（8090）那台文件服务出，开了 `pad_server` 就可能顺带把它
    起起来（`pad_server.media_base_url` 写了外部地址时可不起）；
    ③端口与开关**改了要重启**，设置页没有控件，也不做运行期重开
    （落点与口径见 [src/biz/pad/README.md](src/biz/pad/README.md)）

> 已闭环、不再在此罗列的历史项：解码线程与 `player_widget` 关闭（现 `closeEvent` 两阶段并行停 4 路）、
> 硬解按 `codecpar->codec_id` 选 `h264_rkmpp`/`hevc_rkmpp`、
> 围栏报警改为独立通道级判定 + `geofence.throttleSeconds` 节流（原先传 `bypassThrottle=true` 每帧一条，
> 40 秒能出三百多条，并把同通道的普通检测报警整个吞掉；后来进一步改成**进入沿触发**，
> 站着不动不再每 10 秒重复报）、
> 围栏触发类别改为 `geofence.alarmClass` 单值（V1 的 `alarmClasses` 列表已废弃）、
> 围栏的全局 `mode` 一刀切改为**每形状自带角色**（禁区/岗区域可混用；`mode` 只剩迁移时读一次）、
> 报警类别从槽位属性迁到模型属性、配置里不再出现模型/标签路径（绝对↔相对路径拉锯问题连根消失）、
> 时间戳量纲注释已统一为 epoch 纳秒、`dma_mutex` 跨 TU 单例、
> `QSqlDatabase` 每线程独立连接、NPU 核掩码位标志修正、级联每槽位独立标签与类别数。
> DMA/RGA/命令注入/ns-ms 口径等 2026-09-24 批次的修复明细原载 `plan/update_log.md`（已删，
> 结论都沉淀在上文源码注释与各目录 README 里）。

# 九、文档

- 各模块文档：见上文[目录结构与模块文档索引](#四目录结构与模块文档索引)
- `docs/knowledge/` 技术知识库（详见 [docs/README.md](docs/README.md)）：AlertGateway 技术总结、Docker 部署、H264 封装图解、NPU 全局调度、RGA 预处理加速、YOLOv8s-RK3588 性能优化、NV12 渲染原理、WebRTC 远程桌面、多媒体硬解记录等

# 十、贡献指南

1. Fork 本仓库
2. 创建特性分支 (`git checkout -b feature/AmazingFeature`)
3. 提交更改 (`git commit -m 'Add some AmazingFeature'`)
4. 推送到分支 (`git push origin feature/AmazingFeature`)
5. 创建 Pull Request

# 许可证

本项目未指定许可证，请联系项目维护者获取许可信息。

# 联系方式

项目托管在 GitLab：https://146.56.223.127:30000/iwatcher/idge
