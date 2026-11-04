#ifndef RECOGNITION_RESULT_DIALOG_H
#define RECOGNITION_RESULT_DIALOG_H

/**
 * @file recognition_result_dialog.h
 * @brief 人员点名"登记"识别结果对话框 —— caichao 分支合入
 *
 * 职责：后台线程调用 RollCallService::processPhotos() 做人脸检测+特征提取
 *       +跨照片去重，表格展示每张后处理图与不重复人数；支持人工修正
 *       总人数、上一步重拍、取消(自定义返回码2并删任务)、确认落库。
 */

#include <QDialog>
#include <QTableWidget>
#include <QLabel>
#include <QSpinBox>
#include <QPushButton>
#include <QProgressBar>
#include <QThread>
#include <QVBoxLayout>
#include <QScrollArea>
#include <memory>
#include <set>
#include <vector>
#include "roll_call_service.h"

// TaskProcessResult 需跨线程（识别线程→GUI）经队列连接传递，必须注册元类型
Q_DECLARE_METATYPE(TaskProcessResult)

/**
 * @class RecognitionResultDialog
 * @brief 登记识别结果页。线程模式：QThread(this) + moveToThread Worker，
 *        finished/error 自动 quit、线程 finished 时 deleteLater Worker；
 *        析构 quit()+wait()（识别不可中断，关窗可能短暂无响应，已知取舍）。
 * @warning 遗留死代码警示：drawFaceBoxes() 仅声明、无定义且无调用（若被
 *          调用会直接链接失败）；drawBoxesOnImage() 有定义但全工程无调用
 *          （后处理图的人脸框已由服务层绘制到 processed_path 图片上，
 *          UI 侧二次绘制方案已废弃）。清理前勿当作功能缺失重复实现。
 */
class RecognitionResultDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @brief 建对话框并**推迟启动**后台识别（singleShot(0)）
     * @param task_id   点名任务 id（由 RollCallService 分配；构造时任务已落库）
     * @param photo_paths 待识别的照片绝对路径，来自 PhotoSelectionDialog::getSelectedPhotos()
     * @param service   点名服务；**允许为空**——startRecognition() 判空后弹错并 reject()
     * @param parent    父窗；调用方（RollCallWidget）传 this 并以 exec() 模态使用
     *
     * photo_paths 为空集合不会在这里被拦下：会照常起线程、照常跑 processPhotos，
     * 结果表就是一片空。调用方（RollCallWidget::onCreateTask）已保证非空才往下走。
     */
    explicit RecognitionResultDialog(int task_id, 
                                    const QStringList& photo_paths,
                                    std::shared_ptr<RollCallService> service,
                                    QWidget* parent = nullptr);
    /**
     * @brief 析构：识别线程仍在跑就 quit()+wait()（等不可中断的 NPU 推理结束）
     * ⚠ 关窗可能卡数秒，这是已知取舍；不要改成 terminate()（NPU 上下文会漏）。
     */
    ~RecognitionResultDialog();

signals:
    void taskConfirmed(int task_id);  ///< 确认落库成功后广播（当前调用方靠 exec 返回值，未连接）

private slots:
    void onPrevious();          ///< reject()：回照片选择重拍，任务保留待用户决定
    void onCancel();            ///< 确认后 service 删任务并 done(2)（外层据 2 只提示不再删）
    void onConfirm();           ///< saveTaskResult 成功才 accept()
    void onEditTotalCount();    ///< 总人数标签 <-> QSpinBox(0~10000) 就地切换编辑
    /// 后台线程经**队列连接**回 GUI 线程：这里可以动控件
    /// ⚠ 收到的 result 是值拷贝（TaskProcessResult 已 qRegisterMetaType），
    ///   不要改成传引用/指针——工作线程的对象在信号派发时可能已析构
    void onRecognitionFinished(const TaskProcessResult& result);
    /// 识别失败：藏忙碌条、恢复"上一步/取消"可用（**不恢复确认**），弹错后留在界面
    void onRecognitionError(const QString& message);

private:
    void setupUI();   ///< 一次性搭界面（标题+人数/忙碌条/编辑钮 / 2 列结果表 / 三颗底部钮）
    void startRecognition();  ///< singleShot(0) 延迟启动：先绘制 loading（忙碌条+禁用按钮）
    /// 逐张把后处理图与"不重复人数"填进结果表。
    /// 板端 Qt JPEG 插件与图像流水线用的 libjpeg ABI 冲突，图片**必须**经
    /// loadPixmapSafe 解码；解码失败退化为"已识别 N 张人脸"文字占位而不是崩。
    void displayResults();
    void drawFaceBoxes();     ///< ⚠ 遗留声明：无实现无调用，勿新接
    QPixmap drawBoxesOnImage(const QString& image_path,      ///< ⚠ 遗留实现：无调用（框已由服务层画好）
                            const std::vector<ProcessedFace>& faces,
                            bool is_first_image,
                            const std::set<int>& duplicate_indices);
    
    int task_id_;
    QStringList photo_paths_;
    std::shared_ptr<RollCallService> service_;
    
    TaskProcessResult result_;
    int total_unique_count_;    ///< 可被"编辑人数"人工覆盖，确认后随 result_ 落库
    bool is_editing_count_;     ///< 总人数编辑态（label/spinbox 互斥显隐）
    
    // UI组件
    QLabel* total_count_label_;
    QProgressBar* loading_bar_;       ///< range(0,0) 忙碌指示，识别完成后隐藏
    QThread* recognition_thread_;     ///< parent=this，随对话框析构回收
    QSpinBox* count_spinbox_;
    QPushButton* edit_count_btn_;
    QTableWidget* result_table_;      ///< 2列：后处理图(760x350 缩放,行高400) | 不重复人数
    QPushButton* prev_btn_;
    QPushButton* cancel_btn_;
    QPushButton* confirm_btn_;
};

#endif // RECOGNITION_RESULT_DIALOG_H
