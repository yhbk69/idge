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

class frmMain : public QWidget
{
    Q_OBJECT

public:
    explicit frmMain(QWidget *parent = 0);
    ~frmMain();

protected:
    /**
     * @brief 窗口关闭事件 - 停止所有解码器线程后再关闭
     */
    void closeEvent(QCloseEvent *event) override;

private:
    Ui::frmMain *ui;
    frmVideoWindow *videoWindow;
    DashboardWidget *dashboardWidget_ = nullptr;
    AlarmListWidget *alarmListWidget_ = nullptr;
    VideoAlarmWidget *videoAlarmWidget_ = nullptr;
    QLabel *alarmToast_ = nullptr;   ///< 悬浮报警提示
    QLabel *alarmBadge_ = nullptr;   ///< 未确认报警角标
    QTimer *toastTimer_ = nullptr;   ///< 悬浮提示消失定时器
    QTimer *alarmRefreshTimer_ = nullptr;  ///< 报警面板刷新节流（合并高频报警，防刷屏拖垮 GUI event loop）

    /// 进全屏前那一格的几何，"最大化/还原"按钮退回普通态时用它复位。
    /// 刻意做成成员而不是函数里的 static 局部：这份还原态属于本窗口。
    QRect restoreGeometry_;

    QList<int> iconsMain;          ///< 主导航图标列表
    QList<QAbstractButton *> btnsMain;  ///< 主导航按钮列表

    // ========== 人员点名 / 设备盘点业务模块（caichao 分支合并） ==========
    // 两个业务页不占顶部导航，装在各自的窗口级模态 QDialog 里，
    // 由智能识别页的"人员点名"/"设备盘点"卡片拉起
    RollCallWidget *rollCallWidget_ = nullptr;
    QDialog *rollCallDialog_ = nullptr;      ///< 点名任务页宿主弹窗
    std::shared_ptr<RollCallService> rollCallService_;
    EquipmentInventoryWidget *equipmentWidget_ = nullptr;
    QDialog *equipmentDialog_ = nullptr;     ///< 盘点任务页宿主弹窗
    std::shared_ptr<EquipmentInventoryService> equipmentService_;
    SmartRecognitionWidget *smartRecognitionWidget_ = nullptr; ///< 智能识别页（QR + OCR + 点名 + 盘点）
    QWidget *smartPage_ = nullptr;   ///< 智能识别页容器
    LoginWidget *login_ = nullptr;   ///< 扫码登录页（stackedWidget 的第 0 页，未登录时唯一可见页）
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

    /**
     * @brief 打开视频到指定通道
     * @param channel 通道索引（0-3，对应 videoWindow1~4）
     * @param path    视频文件路径或RTSP地址
     */
    void openVideoToChannel(int channel, const QString &path);

private slots:
    void initForm();               ///< 初始化窗体（无边框、图标、标题、导航）
    void initStyle();              ///< 加载 QSS 样式表并提取颜色
    void initNewPages();           ///< 初始化数据看板和报警记录页面
    void buttonClick();            ///< 顶部导航按钮点击处理
    void showAlarmToast(const AlarmRecord &alarm);  ///< 悬浮报警提示
    void updateAlarmBadge();       ///< 更新未确认报警角标
    void onWsAlarmReceived(const QJsonObject &data);  ///< WebSocket 报警消息处理
    void initLeftMain();           ///< 初始化视频监控页左侧导航
    void leftMainClick();          ///< 视频监控页左侧按钮点击
    void systemExit();             ///< 系统退出确认
    void initDebugPage();          ///< 初始化调试帮助页（从 config.json 读取配置）

    // ========== 扫码登录门（frmMain 只构造一次，登录态是 stackedWidget 的一个页面）==========
    // 门挡的是"检测与报警产生"，不是整个进程的网络出口（GB28181/WS 在门外，见 README）
    void startAllStreams();        ///< 按通道配置开四路；已开则只记日志不重复开（幂等守卫）
    void stopAllStreams();         ///< 停看板刷新 + 两阶段停四路解码（退出登录与关窗共用同一段）
    void showLoginGate();          ///< 合门：显示登录页、收起顶部导航（含角标）、清会话显示
    void onLoginSucceeded();       ///< 开门：恢复导航、进视频监控页、startAllStreams()
    void onLogoutRequested();      ///< 导航「退出登录」：确认 → 停流 → 清限流窗口 → 登出 → 合门
    void appendLog(const QString &msg);  ///< 追加日志到文本框

    // ========== 人员点名 / 设备盘点业务模块 ==========
    void initBusinessPages();      ///< 初始化点名/盘点页面与服务
    bool initRollCallService();    ///< 初始化人员点名服务
    bool initEquipmentService();   ///< 初始化设备盘点服务
    std::vector<EquipmentModelConfig> equipmentModelConfigsFromLibrary(); ///< 从模型库解析盘点清单（init 与 reload 共用同一口径）
    void reloadEquipmentService(); ///< 模型库变更后热更新盘点模型（失败保持旧模型）
    void showRollCallDialog();     ///< 智能识别页"人员点名"卡片：弹出点名任务页
    void showEquipmentDialog();    ///< 智能识别页"设备盘点"卡片：弹出盘点任务页

private:
    // ========== 视频通道配置控件（源/备注/启用/级联模型，通道号一律 0-based） ==========
    QLineEdit *chNoteEdit_[4] = {nullptr, nullptr, nullptr, nullptr};  // 每路通道备注
    QLineEdit *channelSourceEdit(int channel) const;    ///< 通道号(0..3) → 源地址输入框
    void browseChannelSource(int channel, QLineEdit *edit);  ///< 浏览并打开某路视频源

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
     * @brief 让某模型的报警类别改动即时生效（不动模型、不重开流）
     *
     * 只覆盖 video.models[<id>].alarm_class：槽位数没变就能在线换字符串，
     * 解码线程下一帧起按新类别判定报警。若某路的槽位数已与配置不符（模型列表
     * 也改过），这条路走不通，该路退回 applyChannelConfigNow()。
     */
    void applyAlarmClassNow(const QString &libId);

    // ========== 模型参数表（报警类别是"模型的参数"，键 = 模型库目录名） ==========
    QTableWidget *modelParamTable_ = nullptr;
    void initModelParamsUi();       ///< 在通道分组框后插入"模型参数"表格区
    void refreshModelParamsTable(); ///< 重建表格行（库内模型 + config 里残留的键）
    void openChannelModelsDialog(int channel); ///< 勾选并排序该通道的级联模型列表

    // ========== 围栏（独立于级联：类别单值 + 节流窗口） ==========
    void initGeofenceUi();

    // ========== WebSocket 配置控件 ==========
    QLineEdit *wsUrlEdit_ = nullptr;         ///< 服务器地址输入框
    QLabel *wsStatusLabel_ = nullptr;        ///< 连接状态标签
    QPushButton *wsConnectBtn_ = nullptr;    ///< 连接/断开按钮

    // ========== 模型库管理区（P2：model/library 列表 + 导入/删除） ==========
    QTableWidget *modelLibTable_ = nullptr;
    void initModelLibraryUi();       ///< 在"模型配置"分组框后插入管理区
    void refreshModelLibraryTable(); ///< rescan 库并重建表格（含被引用数）
    void importModelViaDialog();     ///< 三步导入：选 rknn → 选 labels → 起 id
    void importModelZip();           ///< zip 模型包一键导入（解压→逐目录 importModel）
    void deleteSelectedModel();      ///< 删除表格选中模型（有引用则拒绝）
    void testSelectedModel();        ///< 用 assets/test/test.jpg 对选中模型单帧推理

private slots:
    // ========== 4路视频浏览按钮 ==========
    void on_btnBrowseCh1_clicked();  ///< 通道1 视频浏览
    void on_btnBrowseCh2_clicked();  ///< 通道2 视频浏览
    void on_btnBrowseCh3_clicked();  ///< 通道3 视频浏览
    void on_btnBrowseCh4_clicked();  ///< 通道4 视频浏览

    // ========== 阈值变化槽 ==========
    void on_spinBoxConfThresh_valueChanged(double value);  ///< 置信度阈值变化
    void on_spinBoxNmsThresh_valueChanged(double value);   ///< NMS阈值变化

private slots:
    void on_btnMenu_Min_clicked();  ///< 最小化窗口
    void on_btnMenu_Max_clicked();  ///< 最大化/还原窗口
    void on_btnMenu_Close_clicked(); ///< 关闭窗口
    void on_pageRoll_customContextMenuRequested(const QPoint &pos);
};

#endif // FRMMAIN_H
