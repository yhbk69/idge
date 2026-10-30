# src/ui/form

> 所属域：**ui 界面域**（目录重组 S4e 迁入）

## 功能概述

`src/ui/form` 是 IDGE（RK3588 施工行为监测与分析系统）的**顶层窗体目录**，包含主窗口 `frmMain` 及其挂载的全部页面控件：

- **视频监控页**：4 路视频 2x2 网格实时显示（`frmVideoWindow` + `src/ui/PlayerWidget` + `src/ui/GLVideoWidget`），含单通道全屏放大与电子围栏绘制工具栏（画之前先选角色：禁区红 / 岗区域绿）；
- **调试设置页**（导航文本"调试设置" → `ui->pageSettings`）：`config.json` 的编辑界面（视频通道/模型/阈值/报警类别/围栏类别）、WebSocket 地址与连接状态、级联模型 5 槽位配置、通道备注与清空，页底是带颜色分类的运行日志（`textBrowserLog`）。配置控件与日志同在一页，全部内嵌在 `frmmain.cpp` 的 `initDebugPage()`/`initCascadeUi()`，没有独立的"系统设置/帮助"页；
- **报警查询页**：数据看板（`dashboard_widget`）与报警记录列表（`alarm_list_widget`），本目录仅负责挂载与角标/toast 联动；
- **人员点名页**（caichao 合入）：任务列表 + 拍照选图 + 登记识别 + 注销匹配全流程；
- **设备盘点页**（caichao 合入）：复用点名选图界面做设备检测登记/注销与详情查看；
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
| frmmain.h/.cpp/.ui | 主窗口：导航、页面装配、config.json 编辑、级联模型配置、日志、报警 toast/角标、业务服务初始化与注入 | main |
| frmvideowindow.h/.cpp/.ui | 4 路视频 2x2 网格窗口 + 围栏绘制工具栏（**角色：禁区/岗区域**、矩形/多边形、删除、清空） | main |
| video_decoder.h/.cpp | FFmpeg 软解码器（录像回放），独立线程 + PTS 帧同步；**当前无实例化点** | main |
| roll_call_widget.h/.cpp | 人员点名任务列表页（创建/查看/注销/删除/导出报告） | caichao 合入 |
| photo_selection_widget.h/.cpp | 拍照/选图对话框（点名与设备盘点共用；GL 预览 + 快门 + 上传） | caichao 合入 |
| recognition_result_dialog.h/.cpp | 登记识别结果对话框（后台线程 processPhotos，含遗留未调用的画框函数） | caichao 合入 |
| cancellation_result_dialog.h/.cpp | 注销匹配结果对话框（后台线程 matchCancellation） | caichao 合入 |
| equipment_inventory_widget.h/.cpp | 设备盘点任务列表页（双服务注入） | caichao 合入 |
| equipment_recognition_dialog.h/.cpp | 设备识别结果对话框（Registration/Cancellation 两阶段共用） | caichao 合入 |
| equipment_detail_dialog.h/.cpp | 设备盘点任务详情对话框（只读快照） | caichao 合入 |
| smart_recognition_widget.h/.cpp | 智能识别页（二维码 + OCR 铭牌文字 + 人员点名 + 设备盘点）：四卡片入口，前两张自带模态对话框，后两张只发请求信号；含匿名 namespace 里的 OCR 选区控件 | 新增 |
| photo_selection_dialog.h | **遗留未使用**旧版选图对话框声明（仅头文件，无实现无引用，含已修正的乱码注释），与 photo_selection_widget.h 同名类互斥 | caichao 合入（废弃版本） |

## 页面与导航结构

- `frmmain.ui` 里 `stackedWidget` 只有三个静态页：`pageMonitor`（视频监控，内容即 `frmVideoWindow`）、
  `pageSettings`（调试设置）、`pageRoll`（**已不再挂载**：人员点名改为智能识别页卡片 + 弹窗，
  该页保留为占位、永不被切到）。早先文档写的 `page1/page2/page4` 是重组前的旧名，已不存在。
- 运行期动态插页（`initNewPages()`/`initBusinessPages()`）：
  - index1 插入 `DashboardWidget`（数据看板）、index2 插入 `AlarmListWidget`（报警数据）；
  - 末尾 `addWidget(smartPage_)`（智能识别，代码创建的容器页）。设备盘点不再有独立页，
    `EquipmentInventoryWidget` 直接装进 `equipmentDialog_`。
- 顶部导航共 6 项，`buttonClick()` 按按钮文本分发：视频监控 / 数据看板 / 报警数据 / 智能识别 / 调试设置 / 退出。
  其中 **智能识别 `btnSmart` 是 `.ui` 里直接新增的按钮**（图标 `:/image/main_smart.png`）；
  导航图标在 `initNewPages()` 里与文本成对重设（`.ui` 里的原始图标语义已与现文本不符），字号统一取 `theme::FS_NAV`。
- **切换按控件指针**（`setCurrentWidget`）而非索引，避免插页导致索引漂移；`buttonClick()` 按按钮文本分发。
- **点名/盘点入口**：`initBusinessPages()` 把两个业务页各装进一个 `QDialog`（`rollCallDialog_`/`equipmentDialog_`，
  窗口级模态 `Qt::WindowModal`），智能识别页卡片经信号触发 `showRollCallDialog()`/`showEquipmentDialog()`，
  每次打开前先 `refreshTaskList()`。用 WindowModal 而非 `exec()` 是硬约束：点名详情是**非模态顶层窗口**，
  应用级模态会把它一起锁死。`btnData`/`btnConfig` 保持 `hide()`，不再复用为业务导航。
- 业务对话框均为**模态**（`exec()`）嵌套：点名创建 = 输入名 → PhotoSelectionDialog → RecognitionResultDialog；自定义返回码 `done(2)` 约定"用户取消且任务已删"，外层据此只提示不重复删除。
  点名/盘点的**任务列表页**是唯一的例外：宿主 `QDialog` 用 `show()` + `Qt::WindowModal`（见上条）。
- 任务详情页（点名）刻意用非模态 `QWidget + WA_DeleteOnClose + show()`，可与列表并行查看。

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
  二维码链路已改为"拍照后对单帧解码"，不再有常驻扫描线程，因此也就没有"先停线程再停解码器"的收尾顺序约束；OCR 仍走 `QProcess` 起外部 demo。
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
- **中文文案编码**：`cancellation_result_dialog.*` 刻意使用 `\uXXXX` 转义书写中文，防源文件编码差异乱码，新增文案请保持该约定。
