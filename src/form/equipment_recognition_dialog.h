#ifndef EQUIPMENT_RECOGNITION_DIALOG_H
#define EQUIPMENT_RECOGNITION_DIALOG_H

/**
 * @file equipment_recognition_dialog.h
 * @brief 设备盘点识别结果对话框 —— caichao 分支合入
 *
 * 职责：在后台线程调用 EquipmentInventoryService::processPhotos() 做
 *       NPU 设备检测，完成后展示"后处理图 + 标签计数"结果表，
 *       支持 上一步/取消(自定义返回码2)/确认保存 三种出口。
 *       Registration 与 Cancellation 两个阶段共用本对话框，仅 phase 不同。
 */

#include <QDialog>
#include <QMetaType>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QStringList>
#include <memory>
#include <map>

#include "equipment_inventory_service.h"

// 跨线程(工作线程→GUI)信号参数必须注册元类型，队列连接才能搬运该结构体
Q_DECLARE_METATYPE(EquipmentTaskResult)

/**
 * @class EquipmentRecognitionDialog
 * @brief 设备识别结果页（线程模式与 RecognitionResultDialog 一致）：
 *        QThread(this) 挂到对话框生命周期，Worker moveToThread 后在
 *        started 信号里执行 run()，finished/error 自动 quit，
 *        finished 时 deleteLater Worker。
 * @warning 析构里 quit()+wait() 兜底：processPhotos 不可中断，
 *          关窗时 GUI 会等待识别线程跑完（最长为全部照片的推理时间），
 *          这是当前实现的已知取舍，勿在识别中强杀线程。
 */
class EquipmentRecognitionDialog : public QDialog {
    Q_OBJECT
public:
    enum Phase { Registration = 0, Cancellation = 1 };  ///< 与 DB phase 列取值一致

    /**
     * @param phase Registration=登记识别(落库为登记数据)，Cancellation=注销比对
     */
    EquipmentRecognitionDialog(int task_id, const QStringList& paths,
                               std::shared_ptr<EquipmentInventoryService> service,
                               Phase phase, QWidget* parent = nullptr);
    ~EquipmentRecognitionDialog() override;

private slots:
    void startRecognition();   ///< 由 singleShot(0) 延迟触发，先让界面绘制出 loading 态
    void onRecognitionFinished(const EquipmentTaskResult& result);
    void onRecognitionError(const QString& message);
    void onPrevious();  ///< reject()：调用方回到照片选择步骤重来
    void onCancel();    ///< 二次确认后 done(2)：调用方据此删除/保留任务
    void onConfirm();   ///< saveResult 成功才 accept()

private:
    void setupUi();
    void renderResults();  ///< 表格逐行放后处理图(760x320 缩放)与标签计数，行高 350
    static QString countsText(const std::map<std::string, int>& counts);

    int task_id_;
    QStringList paths_;
    std::shared_ptr<EquipmentInventoryService> service_;
    Phase phase_;
    EquipmentTaskResult result_;              ///< finished 后缓存，供 onConfirm 保存
    QThread* recognition_thread_ = nullptr;   ///< parent=this，随对话框析构回收
    QLabel* status_label_ = nullptr;
    QLabel* total_label_ = nullptr;
    QProgressBar* progress_ = nullptr;        ///< range(0,0) 忙碌指示（无逐帧进度上报）
    QTableWidget* table_ = nullptr;
    QPushButton* previous_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* confirm_button_ = nullptr;
};

#endif
