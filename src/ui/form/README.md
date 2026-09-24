# src/ui/form

> 所属域：**ui 界面域**（目录重组 S4e 迁入）

## 功能概述

`src/ui/form` 是 IDGE（RK3588 施工行为监测与分析系统）的**顶层窗体目录**，包含主窗口 `frmMain` 及其挂载的全部页面控件：

- **视频监控页**：4 路视频 2x2 网格实时显示（`frmVideoWindow` + `src/ui/PlayerWidget` + `src/ui/GLVideoWidget`），含单通道全屏放大与电子围栏绘制工具栏；
- **系统设置页**：`config.json` 的编辑界面（视频通道/模型/阈值/报警类别/围栏类别）、级联模型 5 槽位配置、通道备注与清空（全部内嵌在 `frmmain.cpp` 的 `initDebugPage()`/`initCascadeUi()`）；
- **报警查询页**：数据看板（`dashboard_widget`）与报警记录列表（`alarm_list_widget`），本目录仅负责挂载与角标/toast 联动；
- **调试帮助页**：page4，运行日志（带颜色分类）+ 上述配置控件；
- **人员点名页**（caichao 合入）：任务列表 + 拍照选图 + 登记识别 + 注销匹配全流程；
- **设备盘点页**（caichao 合入）：复用点名选图界面做设备检测登记/注销与详情查看。

录像回放解码线程 `video_decoder.cpp`（FFmpeg CPU 软解）也在本目录，当前无实例化点（历史方案，实时播放走 `src/media/reader/FFmpegVideoDecoder`）。

## 文件清单

| 文件 | 职责 | 来源 |
|---|---|---|
| frmmain.h/.cpp/.ui | 主窗口：导航、页面装配、config.json 编辑、级联模型配置、日志、报警 toast/角标、业务服务初始化与注入 | main |
| frmvideowindow.h/.cpp/.ui | 4 路视频 2x2 网格窗口 + 围栏绘制工具栏（矩形/多边形/删除/清空） | main |
| video_decoder.h/.cpp | FFmpeg 软解码器（录像回放），独立线程 + PTS 帧同步；**当前无实例化点** | main |
| roll_call_widget.h/.cpp | 人员点名任务列表页（创建/查看/注销/删除/导出报告） | caichao 合入 |
| photo_selection_widget.h/.cpp | 拍照/选图对话框（点名与设备盘点共用；GL 预览 + 快门 + 上传） | caichao 合入 |
| recognition_result_dialog.h/.cpp | 登记识别结果对话框（后台线程 processPhotos，含遗留未调用的画框函数） | caichao 合入 |
| cancellation_result_dialog.h/.cpp | 注销匹配结果对话框（后台线程 matchCancellation） | caichao 合入 |
| equipment_inventory_widget.h/.cpp | 设备盘点任务列表页（双服务注入） | caichao 合入 |
| equipment_recognition_dialog.h/.cpp | 设备识别结果对话框（Registration/Cancellation 两阶段共用） | caichao 合入 |
| equipment_detail_dialog.h/.cpp | 设备盘点任务详情对话框（只读快照） | caichao 合入 |
| photo_selection_dialog.h | **遗留未使用**旧版选图对话框声明（仅头文件，无实现无引用，含已修正的乱码注释），与 photo_selection_widget.h 同名类互斥 | caichao 合入（废弃版本） |

## 页面与导航结构

- `frmmain.ui` 内置 `stackedWidget`，`.ui` 静态页：page1(视频监控，内容即 `frmVideoWindow` 置于 `lab1`)、page2(系统设置)、pageRoll(点名)、page4(调试) 等。
- 运行期动态插页（`initNewPages()`/`initBusinessPages()`）：
  - index1 插入 `DashboardWidget`（数据看板）、index2 插入 `AlarmListWidget`（报警数据）；
  - 末尾 `addWidget(equipPage_)`（设备盘点，代码创建的容器页）。
- **切换按控件指针**（`setCurrentWidget`）而非索引，避免插页导致索引漂移；`buttonClick()` 按按钮文本分发。
- **隐藏按钮复用机制**：导航收敛后 `btnData`/`btnConfig` 先 `hide()` 占位，`initBusinessPages()` 再把文本改为"人员点名/设备盘点"并 `show()`，用它们挂载两个业务页——`.ui` 不改动即实现了导航扩位。
- 业务对话框均为**模态**（`exec()`）嵌套：点名创建 = 输入名 → PhotoSelectionDialog → RecognitionResultDialog；自定义返回码 `done(2)` 约定"用户取消且任务已删"，外层据此只提示不重复删除。
- 任务详情页（点名）刻意用非模态 `QWidget + WA_DeleteOnClose + show()`，可与列表并行查看。

## 使用方法

- **新增页面**：优先代码创建控件 + `stackedWidget->insertWidget/addWidget`，并在 `widgetTop` 借隐藏按钮或新增 `QToolButton` 后在 `buttonClick()` 加文本分支；不要手改 `.ui`。
- **服务注入**：业务页构造函数只做 UI，服务经 `setService(svc)` / `setServices(eq, rc)` 注入（所有权在 `frmMain`，页面持 `shared_ptr`）。顺序约束：**先 `initRollCallService()` 再 `initEquipmentService()`**——设备服务构造依赖点名服务，且两者共用 `roll_call.db` 的 task 表（盘点任务 type=`equipment_registration`），task_id 两侧可互用。服务初始化失败仅弹警告，页面判空降级。
- **后台识别线程模式**：`QThread(this)` + Worker `moveToThread` + `started→run` + finished/error 自动 quit；跨线程携带的结构体必须先 `qRegisterMetaType`；Worker 定义在 .cpp 内时文件尾必须 `#include "xxx.moc"`。
- **IDGE_WORKSPACE 目录约定**（`initBusinessPages()` 读取环境变量，缺省回退当前目录）：
  - `model/face/`：点名三件套（face_recognition 路径、detection.rknn、recognition.rknn）；
  - `model/library/`：设备盘点检测权重（`initEquipmentService()` 经 ModelRegistry 解析，当前用 yolo11n-coco，明火模型入库后自动加入）；
  - `roll_call_data/`：任务目录根与 `roll_call.db`；拍照产物写入各任务 `folder_path`。

## 依赖关系

- `src/biz/service`：RollCallService、EquipmentInventoryService（业务数据与 NPU 识别封装）；
- `src/ui/widgets`：PlayerWidget/GLVideoWidget/DashboardWidget/AlarmListWidget；
- `src/media/reader`：FFmpegVideoDecoder（实时）、CameraPreviewDecoder（MJPEG 预览/拍照）；
- `src/biz/geofence`（经 `src/fence_manager.h` 等）：FenceManager、FenceOverlay、DrawMode；
- `src/base/utils`：qt_image_utils 的 `loadPixmapSafe`（板端 Qt JPEG 插件与 libjpeg ABI 冲突的安全解码，业务页所有图片加载必须走它）；
- 根目录：`ConfigManager`（config.json）、`AlarmManager`、`SharedTypes.hpp`、`core_helper`（IconHelper/QtHelper）；
- CMake 通过 `src/*/*.cpp` GLOB 收集本目录，新增 .cpp 无需改构建脚本。

## 注意事项

- **线程**：三个识别对话框的析构 `quit()+wait()` 会等待不可中断的 NPU 识别结束，关窗可能卡顿数秒（已知取舍）；`frameReady→GLVideoWidget::onFrameReady` 的 `DirectConnection` 仅在解码线程做"加锁换帧 + update()"，GL 绘制仍在 GUI 线程 paintGL；`video_decoder.cpp` 中 QImage 浅封装 FFmpeg 缓冲的覆写/悬垂风险见该文件警示注释。
- **MOC**：带 `Q_OBJECT` 的 Worker 类定义在 .cpp 中时，尾部 `#include "xxx.moc"` 不可删除，否则 vtable 链接失败。
- **.ui 文件**：`frmmain.ui`/`frmvideowindow.ui` 一律不手改；布局微调全部通过代码 `addWidget/insertWidget/moveTo` 实现。
- **遗留文件**：`photo_selection_dialog.h` 未被引用且与 `photo_selection_widget.h` 声明同名类，二者绝不可同时 include；`recognition_result_dialog` 的 `drawFaceBoxes()`（仅声明）与 `drawBoxesOnImage()`（有定义无调用）为死代码，`equipment_detail_dialog` 的 `photoCell()` 同样无调用点，均已加警示注释，清理前需再次全工程检索。
- **中文文案编码**：`cancellation_result_dialog.*` 刻意使用 `\uXXXX` 转义书写中文，防源文件编码差异乱码，新增文案请保持该约定。
