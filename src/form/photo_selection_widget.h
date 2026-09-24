#ifndef PHOTO_SELECTION_WIDGET_H
#define PHOTO_SELECTION_WIDGET_H

/**
 * @file photo_selection_widget.h
 * @brief 拍照/选图对话框（点名登记、点名注销、设备盘点共用）—— caichao 分支合入
 *
 * 职责：QDialog 模态展示，左侧 GL 实时相机预览 + 圆形快门拍照 + 上传本地照片，
 *       右侧纵向列表管理已选照片（每卡片右上角 × 删除），"下一步"停相机并 accept。
 * 注意：类名为 PhotoSelectionDialog，与遗留文件 photo_selection_dialog.h 中
 *       同名类互斥——两者不可同时被包含；当前工程只使用本头文件(+对应 cpp)。
 */

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QStringList>
#include <QCheckBox>
#include <QImage>
#include <QSoundEffect>
#include <memory>

#include "../service/roll_call_service.h"
#include "camera_preview_decoder.h"
#include "../ui/gl_video_widget.h"

/**
 * @class PhotoSelectionDialog
 * @brief 选照对话框。构造入参 detector_configs 为空时，startCamera() 会用
 *        RollCallService 的人脸检测模型兜底（SCRFD, NPU 核0）；设备盘点流程
 *        则显式传入设备检测配置，实现"预览即框出目标"。
 *
 * 线程/所有权要点（详见 cpp 各 connect）：
 *   - CameraPreviewDecoder 自带采集/解码线程；frameReady→GL 渲染用
 *     DirectConnection：槽在解码线程被直接调用，但 onFrameReady 仅在
 *     互斥锁保护下交换帧并 update()（线程安全），真正的 GL 绘制仍
 *     发生在 GUI 线程 paintGL，勿误以为该连接在解码线程操作 GL 上下文；
 *   - photoCaptured（拍照落盘完成）用 Qt::QueuedConnection 回 GUI 线程；
 *   - 析构必须先 stop() 解码器切断回调，再 deleteLater，防止回调打到
 *     已析构的控件上。
 */
class PhotoSelectionDialog : public QDialog {
    Q_OBJECT

public:
    /**
     * @param detector_configs 预览期检测器配置；空=用人脸模型兜底；
     *        模型文件不存在时降级为"无检测纯预览"（qWarning 提示）
     */
    explicit PhotoSelectionDialog(int task_id, 
                                 std::shared_ptr<RollCallService> service,
                                 const std::vector<PreviewDetectorConfig>& detector_configs = {},
                                 QWidget* parent = nullptr);
    ~PhotoSelectionDialog() override;

    QStringList getSelectedPhotos() const { return selected_photos_; }

signals:
    void photosConfirmed(const QStringList& photo_paths);  ///< 无接收者，流程实际靠 exec()+getSelectedPhotos

protected:
    void resizeEvent(QResizeEvent* event) override;  ///< 同步闪白遮罩几何到预览区

private slots:
    void onTakePhoto();     ///< 快门：闪白+音效+生成不重名 capture_*.jpg 后异步落盘
    void onUploadPhotos();  ///< 文件选择器批量加入本地照片（非 jpg 先试解码校验）
    void onNext();          ///< 停止相机→emit photosConfirmed→accept
    void onPhotoCaptured(const QString& path, const QImage& image);  ///< 队列连接回调：入列表+刷缩略图

private:
    void setupUI();
    void startCamera();
    void addPhotoToList(const QString& path, const QImage& image);  ///< 280x210 卡片；image 为空=延迟加载磁盘图
    QPixmap loadPixmapSafe(const QString& path);                    ///< 转发全局同名函数（libjpeg ABI 规避）

    // 服务
    int task_id_;
    std::shared_ptr<RollCallService> service_;
    
    // 相机相关
    CameraPreviewDecoder* camera_decoder_;       ///< this 为 parent；析构中 stop+deleteLater
    std::vector<PreviewDetectorConfig> detector_configs_;
    GLVideoWidget* camera_preview_;              ///< 最小 960x540（16:9），为底部控制条留高
    QLabel* shutter_overlay_;                    ///< 拍照白色闪光遮罩（盖在预览上）
    QSoundEffect* shutter_sound_;                ///< qrc:/shutter.wav 快门音
    
    // 照片数据
    QStringList selected_photos_;                ///< 已选照片绝对路径（accept 时交还调用方）
    
    // UI组件 - 左侧控制
    QPushButton* upload_btn_;
    QPushButton* snap_btn_;                      ///< 圆形自绘快门按钮，拍照期间禁用防重入
    QPushButton* next_btn_;                      ///< 有照片才可用
    QLabel* thumbnail_label_;                    ///< 128x92 最近一张缩略图
    
    // UI组件 - 右侧照片列表
    QScrollArea* photo_list_scroll_;
    QWidget* photo_list_container_;
    QVBoxLayout* photo_list_layout_;             ///< 末尾常挂一个 stretch 占位，插入前先移除
};

#endif // PHOTO_SELECTION_WIDGET_H
