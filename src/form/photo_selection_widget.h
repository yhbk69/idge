#ifndef PHOTO_SELECTION_WIDGET_H
#define PHOTO_SELECTION_WIDGET_H

#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QStringList>
#include <QCheckBox>
#include <QMap>
#include <QImage>
#include <QSoundEffect>
#include <memory>

#include "../service/roll_call_service.h"
#include "../reader/camera_preview_decoder.h"
#include "../ui/gl_video_widget.h"

class PhotoSelectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit PhotoSelectionDialog(int task_id, 
                                 std::shared_ptr<RollCallService> service,
                                 const std::vector<PreviewDetectorConfig>& detector_configs = {},
                                 QWidget* parent = nullptr);
    ~PhotoSelectionDialog() override;

    QStringList getSelectedPhotos() const { return selected_photos_; }

signals:
    void photosConfirmed(const QStringList& photo_paths);

protected:
    void resizeEvent(QResizeEvent* event) override;

private slots:
    void onTakePhoto();
    void onUploadPhotos();
    void onNext();
    void onPhotoCaptured(const QString& path, const QImage& image);

private:
    void setupUI();
    void startCamera();
    void addPhotoToList(const QString& path, const QImage& image);
    QPixmap loadPixmapSafe(const QString& path);

    // 服务
    int task_id_;
    std::shared_ptr<RollCallService> service_;
    
    // 相机相关
    CameraPreviewDecoder* camera_decoder_;
    std::vector<PreviewDetectorConfig> detector_configs_;
    GLVideoWidget* camera_preview_;
    QLabel* shutter_overlay_;
    QSoundEffect* shutter_sound_;
    
    // 照片数据
    QStringList selected_photos_;
    
    // UI组件 - 左侧控制
    QPushButton* upload_btn_;
    QPushButton* snap_btn_;
    QPushButton* next_btn_;
    QLabel* thumbnail_label_;
    
    // UI组件 - 右侧照片列表
    QScrollArea* photo_list_scroll_;
    QWidget* photo_list_container_;
    QVBoxLayout* photo_list_layout_;
};

#endif // PHOTO_SELECTION_WIDGET_H
