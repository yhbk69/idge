# src/ui/form

> 所属域：**ui 界面域**（目录重组 S4e 迁入）

## 功能概述

`src/ui/form` 是 IDGE（RK3588 施工行为监测与分析系统）的**顶层窗体目录**，包含主窗口 `frmMain` 及其挂载的全部页面控件：

- **视频监控页**：4 路视频 2x2 网格实时显示（`frmVideoWindow` + `src/ui/PlayerWidget` + `src/ui/GLVideoWidget`），含单通道全屏放大与电子围栏绘制工具栏（画之前先选角色：禁区红 / 岗区域绿）；
- **调试设置页**（导航文本"调试设置" → `ui->pageSettings`）：`config.json` 的编辑界面（视频通道/模型/阈值/报警类别/围栏类别）、WebSocket 地址与连接状态、级联模型 5 槽位配置、通道备注与清空，页底是带颜色分类的运行日志（`textBrowserLog`）。配置控件与日志同在一页，全部内嵌在 `frmmain.cpp` 的 `initDebugPage()`/`initCascadeUi()`，没有独立的"系统设置/帮助"页；
- **报警查询页**：数据看板（`dashboard_widget`）与报警记录列表（`alarm_list_widget`），本目录仅负责挂载与角标/toast 联动；
- **人员点名页**（caichao 合入）：任务列表 + 拍照选图 + 登记识别 + 注销匹配全流程；
- **设备盘点页**（caichao 合入）：复用点名选图界面做设备检测登记/注销与详情查看；
- **扫码登录页（门禁，本轮新增）**：`LoginWidget`，程序启动后**第一个页面**，未登录不放行主界面；
- **智能识别页**：二维码（ZBar）/ 铭牌文字（PP-OCRv4）/ 人员点名 / 设备盘点 四张卡片。
  前两张点击弹本文件内的模态取流对话框；后两张只 `emit rollCallRequested/equipmentRequested`，
  由 `frmMain` 把对应业务页装进窗口级模态 `QDialog` 弹出。页面本身无常驻任务。
  两个业务页**不再占用顶部导航**（导航 6 项）。
  二维码/OCR 的取帧对话框用 `FFmpegVideoDecoder(nullptr, InferenceMode::PreviewOnly)`：
  只要一帧原始画面给 ZBar / ppocr demo，不加载监控流水线那套 YOLO 级联模型（NPU 零占用，
  预览上也因此没有检测框）。点名/盘点走 `CameraPreviewDecoder`，各自只带自己需要的模型。

录像回放解码线程 `video_decoder.cpp`（FFmpeg CPU 软解）也在本目录，当前无实例化点（历史方案，实时播放走 `src/media/reader/FFmpegVideoDecoder`）。

## 文件清单

| 文件 | 职责 | 来源 |
|---|---|---|
| frmmain.h/.cpp/.ui | 主窗口：导航、页面装配、config.json 编辑、级联模型配置、日志、报警 toast/角标、**标题栏右上角登录人角标**、业务服务初始化与注入 | main |
| frmvideowindow.h/.cpp/.ui | 4 路视频 2x2 网格窗口 + 围栏绘制工具栏（**角色：禁区/岗区域**、矩形/多边形、删除、清空） | main |
| video_decoder.h/.cpp | FFmpeg 软解码器（录像回放），独立线程 + PTS 帧同步；**当前无实例化点** | main |
| roll_call_widget.h/.cpp | 人员点名任务列表页（创建/查看/注销/删除/导出报告） | caichao 合入 |
| photo_selection_widget.h/.cpp | 拍照/选图对话框（点名与设备盘点共用；GL 预览 + 快门 + 上传） | caichao 合入 |
| recognition_result_dialog.h/.cpp | 登记识别结果对话框（后台线程 processPhotos，含遗留未调用的画框函数） | caichao 合入 |
| cancellation_result_dialog.h/.cpp | 注销匹配结果对话框（后台线程 matchCancellation） | caichao 合入 |
| equipment_inventory_widget.h/.cpp | 设备盘点任务列表页（双服务注入） | caichao 合入 |
| equipment_recognition_dialog.h/.cpp | 设备识别结果对话框（Registration/Cancellation 两阶段共用） | caichao 合入 |
| equipment_detail_dialog.h/.cpp | 设备盘点任务详情对话框（只读快照） | caichao 合入 |
| login_widget.h/.cpp | **扫码登录页（门禁）**：**左品牌块 + 右操作列**两栏（2026-10-30 触屏复核后改）——左边 IDGE/「施工行为监测与分析」+ 状态行 + 取景说明，右边只有三颗竖排大按钮（扫码登录 / 测试登录 / 退出程序，480px 宽 × 72px 高）。只发信号 `loginSucceeded/loginFailed(reason)/exitProgramRequested`，不碰 `frmMain` 内部；解析校验全交给 `ConstructionSession` | 新增 |
| smart_recognition_widget.h/.cpp | 智能识别页（二维码 + OCR 铭牌文字 + 人员点名 + 设备盘点）：四卡片入口，前两张自带模态对话框，后两张只发请求信号；含匿名 namespace 里的 OCR 选区控件。**二维码那三行现在只是转调 `uiwidgets` 的 `QrScanDialog::openFullScanner()`**（与登录页共用同一个扫码器） | 新增 |
| photo_selection_dialog.h | **遗留未使用**旧版选图对话框声明（仅头文件，无实现无引用，含已修正的乱码注释），与 photo_selection_widget.h 同名类互斥 | caichao 合入（废弃版本） |

## 页面与导航结构

- `frmmain.ui` 里 `stackedWidget` 只有三个静态页：`pageMonitor`（视频监控，内容即 `frmVideoWindow`）、
  `pageSettings`（调试设置）、`pageRoll`（**已不再挂载**：人员点名改为智能识别页卡片 + 弹窗，
  该页保留为占位、永不被切到）。早先文档写的 `page1/page2/page4` 是重组前的旧名，已不存在。
- 运行期动态插页（`initNewPages()`/`initBusinessPages()`）：
  - index1 插入 `DashboardWidget`（数据看板）、index2 插入 `AlarmListWidget`（报警数据）；
  - 末尾 `addWidget(smartPage_)`（智能识别，代码创建的容器页）。设备盘点不再有独立页，
    `EquipmentInventoryWidget` 直接装进 `equipmentDialog_`。
- 顶部导航共 6 项，`buttonClick()` 按按钮文本分发：视频监控 / 数据看板 / 报警数据 / 智能识别 / 调试设置 / **退出登录**。
  其中 **智能识别 `btnSmart` 是 `.ui` 里直接新增的按钮**（图标 `:/image/main_smart.png`）；
  导航图标在 `initNewPages()` 里与文本成对重设（`.ui` 里的原始图标语义已与现文本不符），字号统一取 `theme::FS_NAV`。
- ⚠ **`btnExit` 是按对象身份分发的**（`if (b == ui->btnExit)`，放在文字链之前），并且被排除在
  `setChecked` 循环外。原因：`buttonClick()` 整条链按**按钮文本**匹配，把"退出"改成"退出登录"
  会让老的 `name == "退出"` 分支静默失配——按钮看着在、点了没反应，是最难查的一类坏法。
  不参与 `setChecked` 是因为它不"选中某个页"，否则退出登录之后它一直高着。
- **切换按控件指针**（`setCurrentWidget`）而非索引，避免插页导致索引漂移；`buttonClick()` 按按钮文本分发。
- **点名/盘点入口**：`initBusinessPages()` 把两个业务页各装进一个 `QDialog`（`rollCallDialog_`/`equipmentDialog_`，
  窗口级模态 `Qt::WindowModal`），智能识别页卡片经信号触发 `showRollCallDialog()`/`showEquipmentDialog()`，
  每次打开前先 `refreshTaskList()`。用 WindowModal 而非 `exec()` 是硬约束：点名详情是**非模态顶层窗口**，
  应用级模态会把它一起锁死。`btnData`/`btnConfig` 保持 `hide()`，不再复用为业务导航。
  **两个宿主 `QDialog` 底部各带一颗 T1 `关闭` 钮**（2026-11-01 触屏批次）：WindowModal 下原本
  唯一出口是 mutter 服务端装饰的 ×，那颗 × 约 24~28px 且**不是 QSS 能管的尺寸**（皮肤层再怎么抬
  也抬不到它），触屏按不到就等于把人锁在模态窗里。尺寸**不可测**（WM 画的，探针读不到）。
- 业务对话框均为**模态**（`exec()`）嵌套：点名创建 = 输入名 → PhotoSelectionDialog → RecognitionResultDialog；自定义返回码 `done(2)` 约定"用户取消且任务已删"，外层据此只提示不重复删除。
  点名/盘点的**任务列表页**是唯一的例外：宿主 `QDialog` 用 `show()` + `Qt::WindowModal`（见上条）。
- 任务详情页（点名）刻意用非模态 `QWidget + WA_DeleteOnClose + show()`，可与列表并行查看。

## 登录门禁状态机

**`frmMain` 只构造一次，登录态是 `stackedWidget` 的第 0 页**，不是"先弹框再建主窗"。
否决过的替代方案（在 `main()` 里先 `exec` 一个登录框）有三个硬伤：`ConfigManager::load()`
发生在 `initForm()` 第 9 步，登录框在那之前拿不到摄像头地址；GB28181 在 `main.cpp` 里已经起过，
"登录前什么都不跑"这个不变量在那个方案下本来就不成立；而"退出登录要能回到登录页"就得反复构造
`frmMain` ⇒ metaType 重复注册、报警信号重复连接、四路重复开。

状态转移（`showLoginGate()` / `onLoginSucceeded()` / `onLogoutRequested()`）：

```
启动 ──► 登录页(0) ──测试登录/扫码成功──► 主页 + startAllStreams()
             ▲                                    │
             └────── 退出登录（btnExit）◄─────────┘
                        │
                        └─ stopAllStreams → resetThrottles → logout → showLoginGate
```

- **光切页面挡不住导航**：`.ui` 里 `widgetTitle{ widgetTop{btnMain…btnExit}, widgetMenu }`
  与 `stackedWidget` 是**平级**的，登录页盖不住那一行。所以 `showLoginGate()` 必须
  `ui->widgetTop->hide()`（报警角标是 `btnEquipCheck` 的子控件，跟着一起藏），登录成功再 `show()`。
  这不只是外观问题：留着就能点「智能识别」，那一页会去开 `/dev/video22`，
  与登录页的扫码器**抢同一个设备节点**。标题栏三钮（`widgetMenu`）留着——× 就是退出程序，
  与登录页那颗按钮同义，不冲突。
- **右上角那颗「登录人」是唯一的常驻登录态显示**：`sessionBadge_` 是 `widgetTitle` 的直接子件，
  用 `insertWidget(count()-1, …)` 插到三钮区**之前**（贴着右上角，且不写死下标）。
  它刻意**不在** `widgetTop` 里：进了 `widgetTop` 就会被第 6 步那句
  `findChildren<QAbstractButton*>` 批量设成导航项、并被 `buttonClick()` 的文字分派接管，
  点一下就会去切页面。文本**只放负责人姓名**一项（2026-11-02：原先是「登录人 姓名 · 施工id」，
  18px 字加 id 在三字姓名上就开始截字），其余四项进
  `uiwidgets` 的 `ConstructionDetailDialog`——那颗钮本身就是详情框的入口。
  可见性由 `updateSessionBadge()` 按会话决定（未登录整颗隐藏），在 `showLoginGate()` 与
  `onLoginSucceeded()` 两处各调一次；`widgetTop` 的 `hide()` 盖不到它，所以必须单独处理，
  否则登录页右上角会挂着一颗"登录人 —"，看着像还登着。
  **为什么非要有这颗**：报警的归属是在 `makeAlarmRecord()` 定死的，换班没点「退出登录」就会
  继续记到上一班名下，而此前界面上没有任何地方能看出"现在登的是谁"——只有一行会滚走的日志。
- **`startAllStreams()` / `stopAllStreams()` 是从 `initForm()` 第 11 步和 `closeEvent()` 里
  抽出来的同一段代码**（纯搬运，不改行为），加 `streamsRunning_` 守卫：重复调用只记一条日志、
  不动作。门要"不开"也要"停了再开"，所以必须先抽出来。实测：登录→退出→再登录，
  日志里 `decoder pacer ch` 恰好 4+4 行，四路各被打开一次。
- **退出登录清什么**：停四路、`AlarmManager::resetThrottles()`、`ConstructionSession::logout()`、
  收掉 toast 与角标。**不清**：DB 历史、`FenceManager` 的围栏（配置态不是会话态）、
  WsClient 连接与**待发上行队列**、备份/关机定时器。
  `resetThrottles()` 不是可选的——限流态是一个内存 map，不清的话重登后 2s（类别）/10s（围栏）
  窗口内的第一条报警会被静默吞掉。上行队列不清是对的：施工信息在 `makeAlarmRecord()` 就定死了，
  发送时不再回查会话，所以重传的那条仍带着**它产生时**的归属。
- **门挡的是"检测与报警产生"，不是整个进程的网络出口**：GB28181 在 `main.cpp` 里按配置启动、
  WsClient 照连接、备份定时器照跑。它配置驱动、板上无 `gb28181` 段即静默跳过、不占摄像头、不产报警，
  挪进门内要改 `main()` 的作用域与 `unique_ptr` 生命周期，风险大于收益。如实写在这里，别当成"登录后才联网"。
- **测试登录与扫码是同一条路**：`plan/construction_qr.json`（gitignore，不入库）读进来后
  喂给与扫码完全相同的 `startLogin(qrText, &err)`，没有第二套解析/校验分支。
  载荷按 `$IDGE_PLAN_DIR` → `plan/` → `../plan/` → `../../plan/` → `../../../plan/` →
  `data/construction_qr.json` 顺序找：`RuntimePaths` 全是相对 CWD 的，而 `run.sh` 会先 cd 进
  `build/build_rk3588_linux`，所以仓库根的 `plan/` 在运行时是**够不着的**（实测命中 `../../plan/`），
  最后那条 `data/` 才是部署机上的真正落点。一个都没找到就把**每个试过的路径**列在中文提示里，
  不静默、不崩。
- **验证扫码只认真摄像头，测试数据只走「测试登录」**：不要把 `smartRecognition.camera` 临时指到
  一个含二维码的视频文件来"验扫码"——那是虚拟摄像头，验的是"解码器读文件"那条路，
  摄像头 + 光照 + 对焦 + 取景这一整条现场链路一步都没走到。要喂假数据就用登录页那颗
  「测试登录」按钮（与扫码同一条 `startLogin()`），要验扫码就用真摄像头对真码。
  板端因此有一条**留给现场**的未验项：真码举到 CSI 摄像头前能否在 1~2 秒内解出
  （取决于码的尺寸与光照，开发侧无法代验）。
- **登录页不常驻摄像头预览**：预览只在扫码窗格打开期间存在，窗格本身是**打开即实时扫描**
  （没有快门键；解出码后冻结画面显示内容，点「确认登录」才关窗格）。实测停在登录页时
  进程的 `/dev/video*` 句柄数为 0、CPU 0%、`alarms` 表 60s 不增；点「扫码登录」后为 1，
  点「取消」或「确认登录」关窗格后又回到 0（本轮板端复测过一遍）。
  这也是"门挡着的时候不碰摄像头"能成立的前提。

## 使用方法

- **新增页面**：优先代码创建控件 + `stackedWidget->insertWidget/addWidget`，并在 `widgetTop` 借隐藏按钮或新增 `QToolButton` 后在 `buttonClick()` 加文本分支；不要手改 `.ui`。
- **服务注入**：业务页构造函数只做 UI，服务经 `setService(svc)` / `setServices(eq, rc)` 注入（所有权在 `frmMain`，页面持 `shared_ptr`）。顺序约束：**先 `initRollCallService()` 再 `initEquipmentService()`**——设备服务构造依赖点名服务，且两者共用 `roll_call.db` 的 task 表（盘点任务 type=`equipment_registration`），task_id 两侧可互用。服务初始化失败仅弹警告，页面判空降级。
- **后台识别线程模式**：`QThread(this)` + Worker `moveToThread` + `started→run` + finished/error 自动 quit；跨线程携带的结构体必须先 `qRegisterMetaType`；Worker 定义在 .cpp 内时文件尾必须 `#include "xxx.moc"`。
- **IDGE_WORKSPACE 目录约定**（`initBusinessPages()` 读取环境变量，缺省回退当前目录）：
  - `model/face/`：点名两件套权重（detection.rknn、recognition.rknn，进程内 `InProcessFaceRecognizer` 直接加载；face_recognition exe 仅 CLI 对照工具，不再需要）；
  - `model/library/`：设备盘点检测权重（`initEquipmentService()` 经 ModelRegistry 解析，当前用 yolo11n-coco，明火模型入库后自动加入）；
  - `roll_call_data/`：任务目录根与 `roll_call.db`；拍照产物写入各任务 `folder_path`。

## 依赖关系

- `src/biz/service`：RollCallService、EquipmentInventoryService（业务数据与 NPU 识别封装）；
- `src/ui/widgets`：PlayerWidget/GLVideoWidget/DashboardWidget/AlarmListWidget；
- `src/media/reader`：FFmpegVideoDecoder（实时）、CameraPreviewDecoder（摄像头预览/拍照，取流地址同 `smartRecognition.camera`）；
- `src/biz/geofence`（经 `src/fence_manager.h` 等）：FenceManager、FenceOverlay、DrawMode、FenceRole；
  视频监控页的角色按钮**必须放在独立的 `fenceRoleGroup_`**，不能塞进 `fenceDrawGroup_`——
  后者的 id 空间 0/1/2 在 `onFenceToolClicked` 里被直接翻译成 `DrawMode`，混进去会让"点角色按钮"变成"换绘制工具"。
- `src/base/imaging`：qt_image_utils 的 `loadPixmapSafe`（板端 Qt JPEG 插件与 libjpeg ABI 冲突的安全解码，业务页所有图片加载必须走它）；
- `src/ui/form/smart_recognition_widget`：二维码链路依赖 `3rdparty/zbar`（编译期 `HAS_ZBAR`，见 `3rdparty/README.md`），OCR 链路用 `QProcess` 起 `3rdparty/ppocrv4/bin` 的两个 rknn_model_zoo demo（不链接，见 `3rdparty/ppocrv4/README.md`），权重在 `model/library/ppocrv4-ocr/`；取流地址读 `config.json` 的 `smartRecognition.camera`；
- 根目录：`ConfigManager`（config.json）、`AlarmManager`、`SharedTypes.hpp`、`src/ui/core`（IconHelper/QtHelper/theme/AppInit）；
- CMake 通过 `src/*/*.cpp` GLOB 收集本目录，新增 .cpp 无需改构建脚本。

## 注意事项

- **线程**：三个识别对话框的析构 `quit()+wait()` 会等待不可中断的 NPU 识别结束，关窗可能卡顿数秒（已知取舍）；`frameReady→GLVideoWidget::onFrameReady` 的 `DirectConnection` 仅在解码线程做"加锁换帧 + update()"，GL 绘制仍在 GUI 线程 paintGL；`video_decoder.cpp` 中 QImage 浅封装 FFmpeg 缓冲的覆写/悬垂风险见该文件警示注释。
- **智能识别页的对话框延时**：`smart_recognition_widget.cpp` 里若用 `QTimer::singleShot` 延时弹窗，都必须带 `&dialog` 作上下文对象——回调按引用捕获了 dialog 的栈上成员，缺上下文时"识别成功到弹窗之间关掉对话框"会解引用野指针直接 SIGSEGV（实测复现过）。
  二维码链路是"窗格打开即实时扫描"：解码跑在**解码线程**上（`frameReady` 直连 + 250ms 限流），
  没有第二条扫描线程，但收尾顺序仍然不能调——**先断扫描回调 → `decoder->stop()`（join 解码线程）
  → `QCoreApplication::sendPostedEvents(&dialog, QEvent::MetaCall)` 把已投递但没派发的结果就地跑完**，
  因为 dialog 与它回调捕获的栈上成员在函数返回后就没了；OCR 仍走 `QProcess` 起外部 demo。
- **MOC**：带 `Q_OBJECT` 的 Worker 类定义在 .cpp 中时，尾部 `#include "xxx.moc"` 不可删除，否则 vtable 链接失败。
- **.ui 文件**：`frmmain.ui`/`frmvideowindow.ui` 一律不手改；布局微调全部通过代码 `addWidget/insertWidget/moveTo` 实现。
- **遗留文件**：`photo_selection_dialog.h` 未被引用且与 `photo_selection_widget.h` 声明同名类，二者绝不可同时 include；`recognition_result_dialog` 的 `drawFaceBoxes()`（仅声明）与 `drawBoxesOnImage()`（有定义无调用）为死代码，`equipment_detail_dialog` 的 `photoCell()` 同样无调用点，均已加警示注释，清理前需再次全工程检索。
- **触屏命中区**：本目录的界面按**板载 10 寸触摸屏、1920x1200 实测**（≈226 PPI，1mm≈8.9 逻辑 px）设计，
  令牌表与"字号不动、只长命中区"的口径见 `src/ui/core/README.md` 的「触屏命中区设计令牌」。落到本目录的三件事：
  顶部导航钮**已去 icon**（纯文字、`FS_NAV_TOUCH`、min-width 112），`widgetMenu` 在代码里覆盖到 160px
  （`.ui` 里锁的 100px 装不下 T4 档三钮），**双击标题栏切最大化已删除**——触屏误触源。
  （原先附带的第二条理由"窗口尺寸一变围栏像素坐标就漂"2026-10-30 起已不成立，围栏顶点改存归一化坐标。）
  报警角标 `alarmBadge_` 与导航钮是耦合的：导航从"图上字下"变纯文字后文字矩形上移，
  `refreshAlarmBadge()` 必须按新钮尺寸重算 `move()`，否则角标压字（实测过，探针看不见这种重叠）。
- **「模型参数」表是一行一个**库 id，不是一行一个通道：6 列 = `模型库id / 报警类别 / 置信度 /
  备注 / 被引用通道 / 操作`。这张表是 `QTableWidget(0, 6)`，**每个格子按绝对列号写**、
  resize 模式也按列索引设，加一列要把后面的列顺次挪位（漏一处就是 `Stretch` 落到错的列上）。
  - 「置信度」是 `QComboBox` 档位点选（`(跟随全局)` + 0.05…0.95 共 20 项）。**为什么不是输入框**：
    本轮不做虚拟键盘，任何输入框在十寸触屏上落不了地 —— 隔壁那两颗全局 `QDoubleSpinBox`
    现在就是"0.25→0.95 要按 14 下 ▲"的状态，所以全局值实际上是手改 JSON 的底座。
  - 手改 JSON 改出的非档位值会多出一条「自定义 X」项**如实显示**，不吸附到最近档位 ——
    否则界面写 0.35、实际跑 0.333，现场只会认为阈值没生效。
  - ⚠ **改阈值不能调 `refreshModelParamsTable()`**：它里面 `reg.rescan()` 会在 GUI 线程重扫
    整库（逐模型 sha256）。`alarm_class` 与 `conf_threshold` 的 handler 都只 apply 不 refresh，跟它。
  - 「被引用通道」那一列不是装饰：阈值按**模型**配而不是按**通道**配，同一个模型挂在几路
    就几路共用一个值，这一列告诉你改动会 fan 到哪几路。
- **改完就生效的那条链路只有一个入口**：`applyModelParamsNow(libId)` → 遍历
  `ConfigManager::modelParamUsers(libId)` → 每路 `pushSlotParamsToChannel(ch)` →
  `FFmpegVideoDecoder::updateSlotParams()`。它内部一次 `resolveChannelSlots(ch)` 同时组出
  报警类与阈值并推下去 —— **一个函数、一次解析、不可能漂移**，别再拆成"类别一条、阈值一条"。
  `updateSlotParams()` 返回 false（槽位数与配置不符）时它退回 `applyChannelConfigNow(ch)` 重开那一路，
  日志会说"通道N 的槽位数与配置不一致，本路按新配置重开"。
  ⚠ 重开有代价（漏一份 NPU 上下文，见 `src/media/reader/README.md`），别在验证时反复走。
- **设置页的落盘纪律**：`ConfigManager::save()` 是**持锁整份 JSON 截断重写**（非原子），
  所以 ① `on_spinBoxConfThresh_valueChanged` 这类 handler 第一句必须是
  `if (qFuzzyCompare(现值, 新值)) return;`（隔壁通道地址编辑器就是这么防的）；
  ② `initDebugPage()` 里给控件灌初值的 `setValue()` 必须 `blockSignals` 包住 ——
  否则"开机进设置页"就白写两次盘。两条都在 2026-10-31 板上用 `config.json` 的 mtime 验过。
- **item-view 的勾选态在 QSS 下不能靠换底色表达**（2026-10-30 逐像素核对，本目录最反直觉的一条）：
  `QListWidget` 一旦挂了 `theme::field()` 的样式表，`QListWidgetItem::setBackground()` 的 brush **被样式表忽略**，
  而 `setForeground()` **仍然生效** —— 于是"选中行底色没变、字却成了黑的"，深底深字直接读不了。
  正确机制是**字重 + 前景亮度**：选中 = `TEXT` 色 + `setBold(true)`，未选 = `TEXT_MUTED` 色不加粗。
  底色交给 QSS，不要在这里跟样式表抢。
- **勾选行要点整行，不能只点靶心**：`item->setFlags(flags & ~Qt::ItemIsUserCheckable)`
  关掉视图自己的切换（勾**仍由 `Qt::CheckStateRole` 画出来**，所以不存在"双切换"bug），
  再在 `itemClicked` 里手动取反 `setCheckState`。靶心是样式给的 ~13px，指肚按不中。
  `indicator` 想真放大只能改 QSS 的 `::indicator` 尺寸（Qt 里那就是命中区）；
  本目录的通道模型选择框两者都做了。
- **按需 new 的对话框要显式挂 QScroller**：`frmMain` 构造末尾那次全量扫描扫不到它们
  （通道模型选择框、OCR 结果框各补了一行 `QtHelper::enableTouchScroll`）。
  挂载范围与 tap/drag 分界的整体口径见 `src/ui/core/README.md`「手指拖动滚动」。
- **日志区自动跟随必须条件化**：`logWithColor()` 只在 `verticalScrollBar()->value() >= maximum()-4`
  时才 `ensureCursorVisible()`。无条件跟随 + 报警约 1 条/秒 = 手指根本滑不动日志。
  2026-10-31 板上按三个方向各验过一次（围栏日志实测约 4 条/秒）：贴底时跟着走、上翻后新日志**不**把视图
  拽回来、划回底部后重新跟随 —— 三条都过，这条不需要改，只是把"验过"记下来免得再当 bug 查一遍。
- **看板不需要"重建后恢复滚动位置"这种代码**：`DashboardWidget` 每 2s 整块重建，实测
  `QScrollArea` 的滚动位置**本来就能跨重建存活**；强行 `setValue()` 恢复反而会跟惯性动画打架。
  这条是本轮做过又撤回的改动，留个记录免得再犯。
- **真全屏归 `frmMain::on_btnMenu_Max_clicked()` 管**：构造里那句
  `this->on_btnMenu_Max_clicked()` 已删（它把窗口摆成 `availableGeometry()`，与全屏冲突），
  槽函数换成 `isFullScreen()` 切换 + `setProperty("canMove", ...)`。为什么非 `_NET_WM_STATE_FULLSCREEN`
  不可（dock 是 shell actor，`setGeometry` 与 `X11BypassWindowManagerHint` 都动不了它）见
  `src/ui/core/README.md`「真全屏」。
- **围栏工具栏**：`frmvideowindow.cpp` 的整条工具栏是内联 QSS（不经 `getStyle()`，多 token 值正常），
  高度 86、钮 T1 档；通道钮固定 56x72，**角色钮不设固定宽**（固定 44px 撞上 `padding:8px 14px` 会吃掉"岗区域"三字）。
  角色语义不能只写 tooltip——触屏 hover 不到，所以文字前挂了 12px 实色方块，
  颜色取 `geofence::FenceOverlay::roleColor`（已提到 public），与画布上围栏同口径、改一处两边同步。
  **工具栏必须 `addWidget(toolbar, 2, 0, 1, 2)` 跨两列**：插进单列会把那一列的最小宽度顶到 ~700px，
  四宫格左右两列立刻不等宽（这就是"四个监控不一样大"的唯一成因）。
  同理**不要在网格里放横向 `QSpacerItem` 撑居左**——弹簧的最小宽度同样会灌进第二列，
  居左交给工具栏自己 `QHBoxLayout` 末尾的 `addStretch()`。
  放大模式（player 占 `(0,0,2,2)`）下跨列写法同样成立，别把它拆回单列。
- **表格行高只能走代码**：`QTableView::item{padding}` 不改行高，必须
  `verticalHeader()->setDefaultSectionSize(theme::TOUCH_TABLE_ROW)`（本目录 2 张 + `alarm_list_widget` 2 张）。
- **行内"容器 + 布局"必须显式给到行高**：`setCellWidget` 的容器按自身 sizeHint 居中放行格、不拉满，
  容器比里面的钮矮就把钮裁掉下半截。点名/盘点任务列表的操作列各吃过一次（板端实测：行 72、
  容器 39、钮 66 ⇒ 只画到 36px，"查看/删除"切一半），现在两处都用一个 `kTaskRowH` 常量同时喂
  `setRowHeight()` 和 `setFixedHeight()`。判据与细节见 `src/ui/core/README.md`「触屏命中区设计令牌」第 6 条。
- **调试设置页的页级按钮走 `theme::mainButton()`，不是 `miniButton()`**：`模型…`/`清空`/`保存`/`连接`/
  `刷新`/`导入模型...`/`导入zip包...`/`测试选中`/`删除选中` 实测原本是 `88x48`（5.4mm，低于 6mm 下限），
  因为 `miniButton` 的 `min-height:30` 打穿了皮肤层的 54。改完 10 处全部 `x72`（8.1mm）。
  通道行那颗 `启用` `QCheckBox` 是另一类：皮肤对 `QCheckBox` **只有 `::indicator` 尺寸、没有整件高度档**，
  所以只能在代码里 `setMinimumHeight(theme::TOUCH_FIELD_H)`（实测 42→56）。indicator 保持 40x40 不变，
  只是从顶格变居中。
- **中文文案编码**：`cancellation_result_dialog.*` 刻意使用 `\uXXXX` 转义书写中文，防源文件编码差异乱码，新增文案请保持该约定。
