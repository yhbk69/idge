#pragma once
/**
 * @file cancellation_result_dialog.h
 * @brief 人员点名"注销"识别结果对话框 —— caichao 分支合入
 *
 * 职责：后台线程调用 RollCallService::matchCancellation() 将注销照片中的
 *       人脸与已登记人脸做特征匹配，表格展示匹配结果（登记脸|注销脸|
 *       说明|相似度），人工核对后确认注销（更新任务状态+落库匹配记录）。
 *       模式与 RecognitionResultDialog 平行，仅数据/阶段不同。
 *
 * 注意：实现中所有中文文案以 QStringLiteral("\uXXXX") 转义书写，
 *       是有意规避源文件编码差异导致的乱码，改写时请保持该约定。
 */
#include <QDialog>
#include <QTableWidget>
#include <QSpinBox>
#include <QProgressBar>
#include <QLabel>
#include <QThread>
#include <QPushButton>
#include <QStringList>
#include <memory>
#include "../service/roll_call_service.h"
/**
 * @class CancellationResultDialog
 * @brief 注销匹配结果页。识别在独立 QThread 中执行（Worker moveToThread 模式，
 *        信号跨线程自动队列连接回 GUI）；析构 quit()+wait() 等待识别线程退出。
 *
 * 与设备/登记结果对话框的一处差异：previous()（上一步）在同一实例内重新拉起
 * 选照并再次 startRecognition()——会 new 第二条 QThread 覆盖旧指针（旧线程此
 * 时已 finished，且两者都以 this 为 parent 随对话框回收，无悬挂 delete，但
 * 旧对象会滞留到对话框销毁，属已知实现取舍）。
 */
class CancellationResultDialog : public QDialog {
 Q_OBJECT
public: CancellationResultDialog(int taskId,const QStringList& paths,std::shared_ptr<RollCallService> service,QWidget* p=nullptr);
    ~CancellationResultDialog() override;
private slots:
    void confirm();               ///< service_->confirmCancellation 成功才 accept()
    void previous();              ///< 重新选照并再次启动匹配（复用本实例）
    void startRecognition();      ///< singleShot(0) 延迟启动，先绘制 loading 态
    void onRecognitionFinished(const CancellationProcessResult& result);
    void onRecognitionError(const QString& message);

private:
    void recognize();  ///< 将 result_.matches 渲染进表格并统计"注销人数"（status==1 计数）
    int taskId_;
    QStringList paths_;
    std::shared_ptr<RollCallService> service_;
    CancellationProcessResult result_;
    QTableWidget* table_ = nullptr;
    QSpinBox* count_ = nullptr;      ///< 注销人数（0~10000），识别完成前禁用，用户可微调
    QProgressBar* loading_ = nullptr;///< range(0,0) 忙碌指示
    QLabel* status_ = nullptr;
    QPushButton* previous_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;   ///< 直接 reject()，不删任务（登记数据仍有效）
    QPushButton* confirm_btn_ = nullptr;
    QThread* recognition_thread_ = nullptr;  ///< parent=this；previous() 会再建一条
};
