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
#include <memory>
#include <string>
#include <vector>

class QAbstractButton;
class QLineEdit;
class frmVideoWindow;
class DashboardWidget;
class AlarmListWidget;
class VideoAlarmWidget;
class QLabel;
class QTimer;
class QWidget;
class RollCallWidget;
class EquipmentInventoryWidget;
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
     * @brief 事件过滤器 - 用于标题栏双击最大化/还原
     */
    bool eventFilter(QObject *watched, QEvent *event);

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

    QList<int> iconsMain;          ///< 主导航图标列表
    QList<QAbstractButton *> btnsMain;  ///< 主导航按钮列表

    // ========== 人员点名 / 设备盘点业务模块（caichao 分支合并） ==========
    RollCallWidget *rollCallWidget_ = nullptr;
    std::shared_ptr<RollCallService> rollCallService_;
    EquipmentInventoryWidget *equipmentWidget_ = nullptr;
    std::shared_ptr<EquipmentInventoryService> equipmentService_;
    QWidget *equipPage_ = nullptr;   ///< 设备盘点页（代码创建，插入 stackedWidget）
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
    void initLeftMain();           ///< 初始化视频监控页左侧导航
    void leftMainClick();          ///< 视频监控页左侧按钮点击
    void systemExit();             ///< 系统退出确认
    void initDebugPage();          ///< 初始化调试帮助页（从 config.json 读取配置）
    void initCascadeUi();          ///< 初始化级联模型配置区（5个槽位+备注+清空）
    void appendLog(const QString &msg);  ///< 追加日志到文本框

    // ========== 人员点名 / 设备盘点业务模块 ==========
    void initBusinessPages();      ///< 初始化点名/盘点页面与服务
    bool initRollCallService();    ///< 初始化人员点名服务
    bool initEquipmentService();   ///< 初始化设备盘点服务
    std::vector<EquipmentModelConfig> equipmentModelConfigsFromLibrary(); ///< 从模型库解析盘点清单（init 与 reload 共用同一口径）
    void reloadEquipmentService(); ///< 模型库变更后热更新盘点模型（失败保持旧模型）

private:
    // ========== 级联模型配置控件（数据成员，不能放 slots 里） ==========
    // P2：路径输入框升级为"模型库下拉框"（数据源 ModelRegistry），
    // custom 路径仍经各行"浏览"按钮进入，以下拉为唯一显示/写入口
    QComboBox *cascadeModelCombo_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    QLineEdit *cascadeNoteEdit_[5] = {nullptr, nullptr, nullptr, nullptr, nullptr};
    QLineEdit *chNoteEdit_[4] = {nullptr, nullptr, nullptr, nullptr};  // 每路通道备注
    QLineEdit *fenceClassesEdit_ = nullptr;  // 围栏报警类别编辑框

    void applyCascadeCombo(int slotIdx);     ///< 下拉当前选项写回 config（槽位0..4）
    void refreshCascadeModelCombos();        ///< 重建 5 个下拉并同步 config 选中项

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

    // ========== 模型/标签浏览按钮 ==========
    void on_btnBrowseModel_clicked(); ///< 模型文件浏览
    void on_btnBrowseLabel_clicked(); ///< 标签文件浏览

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
