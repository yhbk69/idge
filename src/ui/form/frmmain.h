#ifndef FRMMAIN_H
#define FRMMAIN_H

/**
 * @file frmmain.h
 * @brief 主窗口类 - 施工行为监测与分析系统
 *
 * 功能模块：
 *   - 视频监控页：4路视频实时显示（2x2网格）
 *   - 系统设置页：配置子菜单（基本/转发/用户/防区/设备/其他）
 *   - 报警查询页：历史报警记录查询
 *   - 使用帮助页：调试信息显示 + 运行日志
 *
 * 配置管理：
 *   - 所有配置集中在 config.json 中
 *   - 通过 ConfigManager 单例读写配置
 *   - UI 修改自动回写到 config.json
 */

#include <QWidget>
#include <QComboBox>
#include <QTableWidget>
#include <QTextCursor>
#include <QTextCharFormat>
#include <QCloseEvent>
#include <QVector>
#include <QJsonObject>
#include <QPushButton>
#include <memory>
#include <string>
#include <vector>

class QAbstractButton;
class QLineEdit;
class QDialog;
class frmVideoWindow;
class DashboardWidget;
class AlarmListWidget;
class VideoAlarmWidget;
class QLabel;
class QTimer;
class QWidget;
class RollCallWidget;
class EquipmentInventoryWidget;
class SmartRecognitionWidget;
class LoginWidget;
class RollCallService;
class EquipmentInventoryService;
struct EquipmentModelConfig;   // 完整定义见 equipment_inventory_service.h（仅 .cpp 用到）
struct AlarmRecord;

namespace Ui {
class frmMain;
}

/**
 * @class frmMain
 * @brief 唯一的顶层主窗：页面装配点 + 服务装配点 + "配置改完就生效"的唯一出口。
 *
 * 生命周期：**构造一次、活到进程结束**。登录态不是"main() 里先弹登录框再建主窗"，
 * 而是 stackedWidget 的第 0 页（LoginWidget）——反复构造本类会让 qRegisterMetaType、
 * AlarmManager 的信号连接、四路解码各来一遍（否决理由见 src/ui/form/README.md
 * 「登录门禁状态机」）。
 *
 * 职责边界：
 *   - 业务服务（RollCallService / EquipmentInventoryService）在这里创建并以
 *     shared_ptr 注入页面；页面只共享所有权，退出顺序由本类决定；
 *   - config.json 的生效出口只有两类：**要重建槽位的**（模型列表/启用开关/移除
 *     模型条目/改引用的流）一律走 applyChannelConfigNow()，**只改运行期参数的**
 *     （报警类别、置信度、全局阈值）走 applyModelParamsNow()/applyGlobalThresholdNow()。
 *     别再在别处拼第三份"配置快照"，两处各自解析迟早各说各话；
 *   - 报警的 toast / 导航角标 / 面板节流合并显示在这里，报警数据本身在 AlarmManager。
 *
 * 线程要求：本类所有成员函数与槽**只在 GUI 线程**执行。解码线程的检测结果、
 * WebSocket 与 Pad 服务端的日志/报警都是经信号槽（跨线程自动队列连接）投递回来
 * 的，所以槽内可以直接动 ui->；任何"从别的线程直接调本类方法"的写法都不允许。
 *
 * 已知遗留：initLeftMain()/leftMainClick() 与 iconsMain/btnsMain 是模板遗留的死路
 * （.ui 的 widgetLeftMain 里没有按钮，见两边的 NOTE）。
 */
class frmMain : public QWidget
{
    Q_OBJECT

public:
    /**
     * @brief 构造主窗：建 UI → 装页面/服务/信号 → 最后合上登录门。
     * @param parent 顶层窗口，实际调用点恒不传（默认 0 是 .ui 模板遗留写法）
     *
     * 步序里有硬约束，别挪（细节见 .cpp 的 initForm 编号注释）：
     *   - ConfigManager::load() 在 initForm 第 9 步，围栏加载(第10步)、initDebugPage
     *     与 initNewPages()/initBusinessPages() 里所有"读配置建控件"的都排在它后面；
     *   - QtHelper::enableTouchScrollAll(this) 必须在**所有**页面建完之后——
     *     它只扫"此刻已存在"的子件，提前一行就会漏掉大半可滚动区；
     *   - showLoginGate() 是构造的最后一步：门要盖住所有页面，否则登录期间还能
     *     点进智能识别页，那一页会去开与扫码器同一个 /dev/videoNN。
     * 四路视频**不在构造里开**（已挪到 onLoginSucceeded → startAllStreams）。
     */
    explicit frmMain(QWidget *parent = 0);
    /**
     * @brief 只 delete ui。
     *
     * 停解码线程刻意放在 closeEvent()（stopAllStreams()）而不是这里：析构发生时
     * qApp->quit() 已让事件循环结束，那时才 join 线程，在途的队列连接回调就没有
     * 派发点了。先停流、再退事件循环，才能保证 delete ui 拆 PlayerWidget 时
     * 四条解码线程都已退出（Qt 在 QThread 仍运行时被销毁会直接 abort）。
     */
    ~frmMain();

protected:
    /**
     * @brief 窗口关闭事件 - 停止所有解码器线程后再关闭
     */
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::frmMain *ui;                        ///< Designer 生成的界面对象；页面控件一律经它取，不改 .ui
    /// 四宫格视频窗口。**指向 .ui 里那个名字叫 lab1 的提升控件**（frmVideoWindow）：
    /// 名字是 Designer 遗留（当年真是一个 QLabel），别按字面理解成标签，也不拥有它。
    frmVideoWindow *videoWindow;
    DashboardWidget *dashboardWidget_ = nullptr;   ///< 数据看板（动态 insertWidget 到 index1；停止刷新经 stop()）
    AlarmListWidget *alarmListWidget_ = nullptr;   ///< 报警记录列表（动态 insertWidget 到 index2）
    VideoAlarmWidget *videoAlarmWidget_ = nullptr; ///< 视频监控页右侧的实时报警面板（受 400ms 节流驱动）
    QLabel *alarmToast_ = nullptr;   ///< 悬浮报警提示
    QLabel *alarmBadge_ = nullptr;   ///< 未确认报警角标
    QTimer *toastTimer_ = nullptr;   ///< 悬浮提示消失定时器
    QTimer *alarmRefreshTimer_ = nullptr;  ///< 报警面板刷新节流（合并高频报警，防刷屏拖垮 GUI event loop）

    /// 进全屏前那一格的几何，"最大化/还原"按钮退回普通态时用它复位。
    /// 刻意做成成员而不是函数里的 static 局部：这份还原态属于本窗口。
    QRect restoreGeometry_;

    QList<int> iconsMain;          ///< 主导航图标列表（NOTE: 随 btnsMain 一起失效，见下）
    /// NOTE: **当前恒为空**。唯一往它装东西的 .ui 容器 widgetLeftMain 里只有一个
    /// verticalSpacer、且 maximumWidth 锁在 10px（frmmain.ui:408-414），没有可收集的
    /// 按钮。于是 initLeftMain()/leftMainClick() 的循环体一次都不进，
    /// IconHelper::setStyle() 收到空列表 —— 整条"左侧图标导航"是模板遗留的死路。
    /// 现版本左侧那 10px 只是给四宫格留的呼吸位。清理前确认 .ui 不会突然又加按钮。
    QList<QAbstractButton *> btnsMain;  ///< 主导航按钮列表

    // ========== 人员点名 / 设备盘点业务模块（caichao 分支合并） ==========
    // 两个业务页不占顶部导航，装在各自的窗口级模态 QDialog 里，
    // 由智能识别页的"人员点名"/"设备盘点"卡片拉起
    RollCallWidget *rollCallWidget_ = nullptr;      ///< 点名任务列表页（rollCallDialog_ 的内容，非模态复用）
    QDialog *rollCallDialog_ = nullptr;      ///< 点名任务页宿主弹窗
    /// 服务所有权在这里（shared_ptr），页面只是共享；构造顺序有硬约束：
    /// **必须先 initRollCallService() 再 initEquipmentService()**——盘点服务构造时
    /// 依赖点名服务，且两者共用 roll_call.db 的同一张 task 表（盘点任务 type=
    /// "equipment_registration"），task_id 两侧可互用。初始化失败时这里可能为空
    /// （页面判空降级、对应卡片置灰），使用前不必再判，但不要把空指针传给页面。
    std::shared_ptr<RollCallService> rollCallService_;
    EquipmentInventoryWidget *equipmentWidget_ = nullptr; ///< 盘点任务列表页（equipmentDialog_ 的内容）
    QDialog *equipmentDialog_ = nullptr;     ///< 盘点任务页宿主弹窗
    std::shared_ptr<EquipmentInventoryService> equipmentService_;
    SmartRecognitionWidget *smartRecognitionWidget_ = nullptr; ///< 智能识别页（QR + OCR + 点名 + 盘点）
    QWidget *smartPage_ = nullptr;   ///< 智能识别页容器
    LoginWidget *login_ = nullptr;   ///< 扫码登录页（stackedWidget 的第 0 页，未登录时唯一可见页）
    QPushButton *sessionBadge_ = nullptr;  ///< 标题栏右上角的登录人角标（未登录时隐藏），点开=施工详情
    bool streamsRunning_ = false;    ///< 四路是否已在跑（start/stopAllStreams 的幂等守卫）
    std::string workspace_;          ///< 业务资源根目录（model/face、roll_call_data 等）

private:
    // ========== QSS 样式颜色变量 ==========
    QString borderColor;           ///< 边框高亮颜色
    QString normalBgColor;         ///< 正常背景颜色
    QString darkBgColor;           ///< 深色背景颜色
    QString normalTextColor;       ///< 正常文字颜色
    QString darkTextColor;         ///< 深色文字颜色

    /**
     * @brief 从 QSS 样式表中提取单个颜色值
     * @param qss   完整的 QSS 样式表字符串
     * @param flag  颜色标记（如 "TextColor:"）
     * @param color [out] 提取到的颜色值（#RRGGBB 格式）
     */
    void getQssColor(const QString &qss, const QString &flag, QString &color);

    /**
     * @brief 从 QSS 样式表中提取所有颜色值
     */
    void getQssColor(const QString &qss, QString &textColor,
                     QString &panelColor, QString &borderColor,
                     QString &normalColorStart, QString &normalColorEnd,
                     QString &darkColorStart, QString &darkColorEnd,
                     QString &highColor);

    // ========== 日志功能（参考 rknn_Multithread 项目） ==========
    /**
     * @brief 获取当前时间戳字符串
     * @return 格式: "yyyy-MM-dd HH:mm:ss"
     */
    QString currentTimestamp();

    /**
     * @brief 添加日志消息（根据类别自动选择颜色）
     * @param category 日志类别：alarm/error(红), warning(黄), system(蓝), info(绿), 其他(灰)
     * @param message  日志消息内容
     */
    void log(const QString &category, const QString &message);

    /**
     * @brief 添加带颜色的日志消息到 QTextBrowser
     * @param category 日志类别
     * @param message  日志消息
     * @param color    文字颜色
     */
    void logWithColor(const QString &category, const QString &message, const QColor &color);

private slots:
    void initForm();               ///< 初始化窗体（无边框、图标、标题、导航）
    void initStyle();              ///< 加载 QSS 样式表并提取颜色
    /// 装配动态页与全部跨域信号连接（看板/报警列表/登录页/报警面板/toast/角标）
    /// ⚠ 名字有误导：它不只"初始化数据看板和报警记录页面"，本类**几乎所有信号槽
    /// 连接都集中在这一处**（AlarmManager / WsClient / FenceManager / AlarmUplink /
    /// PadWsServer）。新增连接放这里而不是别处，否则同一信号会被连两次。
    /// 顺序前提：必须在 initForm()（已 load 配置）之后调用；内嵌的 lambda 引用的
    /// alarmRefreshTimer_ 等成员在本函数**后半段**才 new 出来——同一趟 GUI 线程里
    /// 信号不会在 connect 与 new 之间派发，所以是安全的，但别把这段拆成两次调用。
    /// 末尾还会起 WsClient 与（按配置的）HttpFileService/PadWsServer，二者 stop
    /// 在 src/main.cpp 的事件循环之后，与本函数刻意不成对。
    /// 按当前配置起一台 Pad WS 服务端（含它依赖的证据文件服务 8090）。
    /// initNewPages 启动时调一次；调试页「保存并应用」重启服务时再调。
    /// 只负责**起**：停由调用方先做（PadWsServer::stop()），媒体服务是进程级的、从不停。
    bool startPadServerFromConfig(QString *err);

    /// 按当前配置起一台 Pad HTTP `/api/` 服务端（v1 §4~§7，默认 8899）。
    /// 与上面那条同一形状：启动时调一次，调试页「保存并应用」先 stop 再调它。
    /// 只负责**起**（含按配置装配路由）；媒体文件服务 8090 仍由 startPadServerFromConfig 那侧管，
    /// 本函数只在它没起来时补起一次（围栏画面与整改照片都从那个口出字节）。
    bool startPadHttpFromConfig(QString *err);

    void initNewPages();
    void buttonClick();            ///< 顶部导航按钮点击处理
    /// 悬浮报警提示：右下角 toast，3 秒自动消失（toastTimer_ 单发，只保留最后一条）
    /// ⚠ 线程：AlarmManager::alarmGenerated 是**解码线程**里 storeAndNotify() 发的
    /// （src/media/reader/ffmpeg_video_decoder.cpp 的 deliverAlarms），本槽经自动
    /// 队列连接回到 GUI 线程后才执行，所以这里能直接动 alarmToast_；
    /// 反过来说，toast 每来一条就重挂一次定时器，多条连发时只显示最后一条，是取舍。
    /// alarm.channel 是 0-based，显示时才 +1。
    void showAlarmToast(const AlarmRecord &alarm);
    /// 更新未确认报警角标（>99 显示 99：角标 min-width 24/max 48，三位数会撑爆钮）
    /// 定位按 btnEquipCheck 当前宽度重算，导航钮尺寸一改就必须跟着改（README）
    void updateAlarmBadge();
    /// WebSocket 报警消息处理：只打日志 + 回 ack，**不入库、不弹 toast**
    /// （对端下发的报警与本机 AlarmManager 是两条独立链路，别在这里再调 showAlarmToast）
    /// timestamp 字段当前只读不用（Q_UNUSED），要按它排序/落库得先补解析
    void onWsAlarmReceived(const QJsonObject &data);
    void initLeftMain();           ///< 初始化视频监控页左侧导航（NOTE: 现为死路，见 btnsMain）
    void leftMainClick();          ///< 视频监控页左侧按钮点击（NOTE: btnsMain 为空，实际什么都不做）
    void systemExit();             ///< 系统退出确认
    /// 初始化调试帮助页（从 config.json 读取配置），并把"改一下就落盘"的 handler 接上
    /// ⚠ 前置：必须在 ConfigManager::load() 之后调用（本函数通篇读 cfg 灌控件）。
    /// ⚠ 落盘纪律：给控件灌初值的那几句 setValue/addItems **必须 blockSignals 包住**，
    ///   否则"开机进设置页"本身就会触发一次 save()（持锁整份 JSON 截断重写、非原子），
    ///   2026-10-31 板上用 config.json 的 mtime 实测过。handler 第一句还要再比一次
    ///   "值是否真的变了"（见 .cpp 各 lambda）。
    /// 末尾会把当前配置的推理任务数、各通道模型、围栏清点打成初始日志（现场核对入口）。
    void initDebugPage();

    // ========== 扫码登录门（frmMain 只构造一次，登录态是 stackedWidget 的一个页面）==========
    // 门挡的是"检测与报警产生"，不是整个进程的网络出口（GB28181/WS 在门外，见 README）
    void startAllStreams();        ///< 按通道配置开四路；已开则只记日志不重复开（幂等守卫）
    /// 停看板刷新 + 两阶段停四路解码（先全部 signalStop、再全部 waitForStop）。
    /// ⚠ 会**阻塞 GUI 线程**直到解码线程真退出（有界：单路最长 3s+10s+1.5s×槽位，
    ///   驱动卡住时会更久）——两阶段就是为了把总等待压成 max(单路) 而不是 4 路之和。
    ///   只在登录成功/退出登录/关窗三处调，不要在报警或帧回调路径上调。
    void stopAllStreams();
    void showLoginGate();          ///< 合门：显示登录页、收起顶部导航（含角标）、清会话显示
    void onLoginSucceeded();       ///< 开门：恢复导航、进视频监控页、startAllStreams()
    void onLogoutRequested();      ///< 导航「退出登录」：确认 → 停流 → 清限流窗口 → 登出 → 合门
    void updateSessionBadge();     ///< 按当前会话刷新右上角登录人角标（未登录则隐藏）
    void openSessionDetail();      ///< 点登录人角标：弹出当前施工信息详情框
    void appendLog(const QString &msg);  ///< 追加日志到文本框

    // ========== 人员点名 / 设备盘点业务模块 ==========
    /// 初始化点名/盘点页面与服务。读 IDGE_WORKSPACE（缺省回退当前工作目录）拼
    /// model/face 与 roll_call_data；**服务初始化顺序是硬约束**（先点名后盘点）；
    /// 两个宿主 QDialog 用 Qt::WindowModal 而不是 exec()：点名详情是非模态顶层窗口，
    /// 应用级模态会把它一起锁死。失败只弹警告、卡片置灰，页面判空降级。
    void initBusinessPages();
    bool initRollCallService();    ///< 初始化人员点名服务
    bool initEquipmentService();   ///< 初始化设备盘点服务
    std::vector<EquipmentModelConfig> equipmentModelConfigsFromLibrary(); ///< 从模型库解析盘点清单（init 与 reload 共用同一口径）
    void reloadEquipmentService(); ///< 模型库变更后热更新盘点模型（失败保持旧模型）
    void showRollCallDialog();     ///< 智能识别页"人员点名"卡片：弹出点名任务页
    void showEquipmentDialog();    ///< 智能识别页"设备盘点"卡片：弹出盘点任务页

private:
    // ========== 视频通道配置控件（源/备注/启用/级联模型，通道号一律 0-based） ==========
    QLineEdit *chNoteEdit_[4] = {nullptr, nullptr, nullptr, nullptr};  // 每路通道备注
    /// 每路引用的视频流下拉。**只在 initDebugPage() 里建**，所以调用点都要判空
    /// （登录门在设置页之前，showLoginGate 阶段这些指针还是 nullptr）。
    QComboBox *chSourceCombo_[4] = {nullptr, nullptr, nullptr, nullptr};  // 每路引用的视频流
    QLineEdit *channelSourceEdit(int channel) const;    ///< 通道号(0..3) → 源地址输入框（已整片隐藏，不再读写）
    void browseChannelSource(int channel);             ///< 浏览本地文件 → 建一条流并给本路引用
    /// @brief 用 config 重灌四路下拉（注册表条目 + 内联/未选两项），不触发任何写盘
    /// 全程 blockSignals：任何一次调用都不该被当成"用户选了某项"去落盘/重开流。
    void refreshChannelSourceCombos();
    /// @brief 某路下拉被用户改动：按选中的那条流改 source_id、落盘并重开本路
    /// 连的是 QComboBox::activated（只有用户动作才发），且每个分支先比"引用是否真变了"
    /// ——重新点中当前项也会发 activated，不判就成了"没改配置却重开了一次流"。
    void applyChannelSourceChoice(int channel);
    /// @brief 打开"视频流管理"对话框；关闭后刷新下拉，并重开受影响的那几路
    /// ⚠ 必须在 exec() **之前**取每路的 channelSourceId 快照：删流时 ConfigManager
    ///   会连带把引用清掉，事后再比就找不到受影响的是哪一路了。
    void openStreamManager();

    // ========== 通道配置热更新（改完不必重启整个程序） ==========
    /**
     * @brief 让某一路按当前 config 重新拉起来：停本路 → 释放推理任务 → 重开
     *
     * 凡改动**需要重建槽位**的配置（通道模型列表、启用开关、移除模型条目）都走这里，
     * 是这类改动的唯一出口。代价：该路重新拉流（本地文件回到开头、RTSP/国标是重连），
     * 停→起之间画面保留最后一帧；等待有界（正常 <1 秒）。
     */
    void applyChannelConfigNow(int channel);
    /**
     * @brief 让某模型的运行期参数改动即时生效（报警类别 + 置信度，不动模型、不重开流）
     *
     * 覆盖 video.models[<id>].alarm_class 与 .conf_threshold：两者都是"槽位的运行期参数"，
     * 解码线程每帧在锁内拷一份去构造报警上下文并塞进 TaskData，下一帧起就是新值。
     * 槽位数没变才可能在线换；若某路的槽位数已与配置不符（模型列表也改过），
     * 这条路走不通，该路退回 applyChannelConfigNow()。
     */
    void applyModelParamsNow(const QString &libId);
    /**
     * @brief 把当前 config 解析出的槽位参数推给某一路；true = 已在线生效
     *
     * applyModelParamsNow() 与「全局阈值」两个入口共用，避免"两处各自拼一份快照"漂移。
     * false 包含两种情况：该路没在跑（下次开流自然读新配置）、形状不符已转整路重开。
     */
    bool pushSlotParamsToChannel(int channel);
    /// @brief 全局 conf/nms 改动推给所有通道（回落链底座变了，所有"跟随全局"的槽位都受影响）
    void applyGlobalThresholdNow();

    // ========== 模型参数表（报警类别与置信度都是"模型的参数"，键 = 模型库目录名） ==========
    /// 一行一个**库 id**（不是一行一个通道），6 列全按绝对列号写格子：
    /// 模型库id / 报警类别 / 置信度 / 备注 / 被引用通道 / 操作。
    /// 加一列必须把 refreshModelParamsTable() 里的 setCellWidget 列号与
    /// initModelParamsUi() 里的 setSectionResizeMode 一起顺次挪（漏一处 = Stretch 落到错的列）。
    QTableWidget *modelParamTable_ = nullptr;
    void initModelParamsUi();       ///< 在通道分组框后插入"模型参数"表格区
    /// 重建表格行（库内模型 + config 里残留的键）。
    /// ⚠ 里面第一句就是 reg.rescan()：**在 GUI 线程重扫整个模型库（逐模型 sha256）**。
    ///   改置信度/报警类别这类"每帧都点得到"的 handler 一律只 apply 不 refresh，
    ///   否则每次点下拉都要重扫一遍库、界面直接卡住（见 README「模型参数」那条）。
    void refreshModelParamsTable();
    /// 勾选并排序该通道的级联模型列表：**列表行序 = 槽位顺序 = result_id = 画面上的 M1/M2**，
    /// 所以必须能上下移动，只勾不排序会静默改变推理链顺序。确定后走整路重开。
    void openChannelModelsDialog(int channel);

    // ========== 围栏（独立于级联：类别单值 + 节流窗口） ==========
    /// 在设置页末尾加"电子围栏"分组：围栏类别（单值，默认 person）+ 报警最小间隔秒数。
    /// 类别候选是**四路所用模型类名的并集**（围栏只问"这一路能不能产出该类别"），
    /// 当前值不在并集里时 prepend 进去，不让配置里的值在界面上凭空消失。
    /// 类别改动会立刻 FenceManager::loadFromConfig()（判定端逐帧现读，不用重开流）。
    void initGeofenceUi();

    // ========== WebSocket 配置控件 ==========
    QLineEdit *wsUrlEdit_ = nullptr;         ///< 服务器地址输入框
    QLabel *wsStatusLabel_ = nullptr;        ///< 连接状态标签
    QPushButton *wsConnectBtn_ = nullptr;    ///< 连接/断开按钮

    // ========== 模型库管理区（P2：model/library 列表 + 导入/删除） ==========
    /// 只读表格（6 列：id/名称/任务/类别数/输入/被引用）。
    /// 「被引用」非 0 时删除会被 ModelRegistry 拒绝——这是删模型的唯一护栏。
    QTableWidget *modelLibTable_ = nullptr;
    void initModelLibraryUi();       ///< 在"模型配置"分组框后插入管理区
    void refreshModelLibraryTable(); ///< rescan 库并重建表格（含被引用数）
    void importModelViaDialog();     ///< 三步导入：选 rknn → 选 labels → 起 id
    void importModelZip();           ///< zip 模型包一键导入（解压→逐目录 importModel）
    void deleteSelectedModel();      ///< 删除表格选中模型（有引用则拒绝；目录移入 model/.trash/，非物理删除）
    /// 「更新选中模型」：换同一 id 的权重文件。旧的那份先归档进 data/model_versions，
    /// 成功后按该 id 的引用通道逐个 applyChannelConfigNow()（权重换了必须重建 NPU 上下文，
    /// applyModelParamsNow 那种"只换运行期参数"的路走不通）。
    void updateSelectedModel();
    /// 「版本/回滚」：列出该 id 的归档版本，选一个复制回库（回滚前的那份同样归档，可反复滚）。
    void rollbackSelectedModel();
    /// 用 assets/test/test.jpg 对选中模型单帧推理。
    /// ⚠ 同步跑在 **GUI 线程**：NPU 加载数秒 + 一次推理，期间整窗无响应（只有 WaitCursor）。
    /// 作为验证工具可接受，但别把它接到任何自动流程里。
    /// 阈值按该库 id 走同一条回落链（resolveConfThreshold），否则会出现
    /// "测试页框一堆、监控页一个不见"，让人误判配置没生效。
    void testSelectedModel();

private slots:
    // ========== 4路视频浏览按钮 ==========
    // 名字是 .ui 的对象名自动连接（on_<控件名>_clicked），四颗钮在 Designer 里是四个
    // 独立对象，所以只能有四个槽；实现全部转调 browseChannelSource(ch)，
    // 通道号在这里 +1 基转 0 基——程序内部一律 0-based。
    void on_btnBrowseCh1_clicked();  ///< 通道1 视频浏览
    void on_btnBrowseCh2_clicked();  ///< 通道2 视频浏览
    void on_btnBrowseCh3_clicked();  ///< 通道3 视频浏览
    void on_btnBrowseCh4_clicked();  ///< 通道4 视频浏览

    // ========== 阈值变化槽 ==========
    /// 置信度阈值变化（全局档 = 回落链的底座）
    /// ⚠ 第一句必须是 qFuzzyCompare 早退：valueChanged 是**每一步**发一次，
    ///   触屏长按 ▼ 会连发，而 save() 是持锁整份 JSON 截断重写（非原子）。
    void on_spinBoxConfThresh_valueChanged(double value);
    /// NMS 阈值变化（只有全局一档：NMS 是跨模型的 IoU 合并口径，每模型再配一列不值）
    /// 防抖口径同上，早退那句不能删。
    void on_spinBoxNmsThresh_valueChanged(double value);

private slots:
    void on_btnMenu_Min_clicked();  ///< 最小化窗口
    void on_btnMenu_Max_clicked();  ///< 最大化/还原窗口
    void on_btnMenu_Close_clicked(); ///< 关闭窗口
    /// NOTE: **当前永远不会被调用**。它靠 .ui 对象名自动连接，而 Qt 只在控件的
    /// contextMenuPolicy == CustomContextMenu 时才发 customContextMenuRequested；
    /// frmmain.ui 里的 pageRoll 没设该属性（默认 DefaultContextMenu），函数体也是空的。
    /// 触屏上本来就没有右键可弹上下文菜单，这条链是双份的死。
    /// pageRoll 这一页现在不再挂载任何业务内容（点名已改为智能识别卡片 + 弹窗），
    /// 保留它只为不改动 .ui（本目录约定：.ui 一律不手改）。要清理就整条一起删。
    void on_pageRoll_customContextMenuRequested(const QPoint &pos);
};

#endif // FRMMAIN_H
