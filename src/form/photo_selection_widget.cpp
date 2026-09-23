/*照片选择对话框
两种照片来源：
拍照：
调用 /dev/video41 摄像头，通过 CameraPreviewDecoder 解码 MJPEG 流
实时预览，点击圆形快门按钮拍照
播放快门音效 + 白色闪屏动画
照片保存到任务文件夹，左下角显示缩略图
上传照片：文件选择器批量添加图片
编辑模式：切换后每张照片右上角显示 "×" 按钮删除
网格展示：每行 3 张，360×360 正方形卡片
*/
// photo_selection_widget.cpp
#include "photo_selection_widget.h"
#include "../utils/qt_image_utils.h"
#include "../reader/camera_preview_decoder.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QFileInfo>
#include <QDateTime>
#include <QImage>
#include <QPixmap>
#include <QTimer>
#include <QDebug>
#include <QSoundEffect>
#include <QUrl>
#include <QCoreApplication>
#include <QDir>
#include <QScrollArea>
#include <QPainter>
#include <QPaintEvent>

namespace {
class RoundCaptureButton final : public QPushButton {
public:
    explicit RoundCaptureButton(QWidget* parent = nullptr)
        : QPushButton(parent) {
        setMouseTracking(true);
        setFocusPolicy(Qt::NoFocus);
    }

protected:
    void paintEvent(QPaintEvent* event) override {
        Q_UNUSED(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing, true);

        const qreal border_width = 7.0;
        const QRectF circle = QRectF(rect()).adjusted(
            border_width / 2.0, border_width / 2.0,
            -border_width / 2.0, -border_width / 2.0);

        QColor fill = QColor(QStringLiteral("#F8FAFC"));
        if (!isEnabled()) {
            fill = QColor(QStringLiteral("#CBD5E1"));
        } else if (isDown()) {
            fill = QColor(QStringLiteral("#94A3B8"));
        } else if (underMouse()) {
            fill = QColor(QStringLiteral("#E2E8F0"));
        }

        painter.setPen(QPen(QColor(QStringLiteral("#CBD5E1")), border_width));
        painter.setBrush(fill);
        painter.drawEllipse(circle);
    }
};
} // namespace

PhotoSelectionDialog::PhotoSelectionDialog(int task_id, std::shared_ptr<RollCallService> service,
                                           const std::vector<PreviewDetectorConfig>& detector_configs,
                                           QWidget* parent)
    : QDialog(parent),
      task_id_(task_id),
      service_(service),
      camera_decoder_(nullptr), detector_configs_(detector_configs) {
    setupUI();
    startCamera();
}

PhotoSelectionDialog::~PhotoSelectionDialog() {
    if (camera_decoder_) {
        camera_decoder_->stop();
        camera_decoder_->deleteLater();
    }
}

void PhotoSelectionDialog::setupUI() {
    setWindowTitle(QStringLiteral("照片识别"));
    resize(1600, 900);
    setMinimumSize(1400, 800);
    setStyleSheet(QStringLiteral("QWidget { background: #05070C; }"));

    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(16, 16, 16, 16);
    main_layout->setSpacing(16);

    // === 左侧：相机预览区域 ===
    auto* left_widget = new QWidget(this);
    left_widget->setStyleSheet(QStringLiteral("QWidget { background: #05070C; }"));
    auto* left_layout = new QVBoxLayout(left_widget);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(16);

    // 顶部工具栏：只有"选择照片"按钮
    auto* toolbar = new QWidget(left_widget);
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(0, 0, 0, 0);

    QString button_style = 
        QStringLiteral("QPushButton {"
        "  background: #38BDF8;"
        "  color: #05070C;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 10px 24px;"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: #0EA5E9; }"
        "QPushButton:pressed { background: #0284C7; }");

    upload_btn_ = new QPushButton(QStringLiteral("选择照片"), toolbar);
    upload_btn_->setMinimumSize(140, 44);
    upload_btn_->setStyleSheet(button_style);
    connect(upload_btn_, &QPushButton::clicked, this, &PhotoSelectionDialog::onUploadPhotos);
    toolbar_layout->addWidget(upload_btn_);
    toolbar_layout->addStretch();

    left_layout->addWidget(toolbar);

    // 相机预览窗口
    camera_preview_ = new GLVideoWidget(left_widget);
    // Keep the 16:9 preview, but leave enough vertical room for the controls
    // on boards whose screen is close to the dialog's minimum height.
    camera_preview_->setMinimumSize(960, 540);
    camera_preview_->setStyleSheet(
        QStringLiteral("background: #0A0D12;"
        "border: 2px solid #161D2B;"
        "border-radius: 12px;"));
    left_layout->addWidget(camera_preview_, 1);

    // 快门闪白效果
    shutter_overlay_ = new QLabel(camera_preview_);
    shutter_overlay_->setGeometry(camera_preview_->rect());
    shutter_overlay_->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 235);"));
    shutter_overlay_->hide();

    // 底部控制栏
    auto* controls = new QHBoxLayout();
    controls->setContentsMargins(8, 0, 8, 8);
    controls->setSpacing(12);

    // 左侧：最近拍摄的缩略图
    thumbnail_label_ = new QLabel(left_widget);
    thumbnail_label_->setFixedSize(128, 92);
    thumbnail_label_->setAlignment(Qt::AlignCenter);
    thumbnail_label_->setStyleSheet(
        QStringLiteral("background: #000000;"
        "border: 2px solid #283244;"
        "border-radius: 8px;"));
    controls->addWidget(thumbnail_label_, 0, Qt::AlignLeft | Qt::AlignVCenter);
    controls->addStretch(1);

    // 中间：圆形拍照按钮
    auto* snap_button_container = new QWidget(left_widget);
    snap_button_container->setFixedSize(80, 80);
    snap_button_container->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);

    snap_btn_ = new RoundCaptureButton(snap_button_container);
    snap_btn_->setObjectName(QStringLiteral("snapButton"));
    snap_btn_->setFixedSize(80, 80);
    snap_btn_->setGeometry(snap_button_container->rect());
    snap_btn_->setToolTip(QStringLiteral("拍照"));
    snap_btn_->setCursor(Qt::PointingHandCursor);
    connect(snap_btn_, &QPushButton::clicked, this, &PhotoSelectionDialog::onTakePhoto);
    controls->addStretch(1);
    controls->addWidget(snap_button_container, 0, Qt::AlignTop);
    controls->addStretch(2);

    // 右侧：下一步按钮
    next_btn_ = new QPushButton(QStringLiteral("下一步"), left_widget);
    next_btn_->setFixedSize(140, 48);
    next_btn_->setEnabled(false);
    next_btn_->setStyleSheet(
        QStringLiteral("QPushButton {"
        "  background: #6EE7B7;"
        "  color: #05070C;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 10px 32px;"
        "  font-size: 15px;"
        "  font-weight: 700;"
        "}"
        "QPushButton:hover { background: #34D399; }"
        "QPushButton:disabled { background: #0F131C; color: #666; }"));
    connect(next_btn_, &QPushButton::clicked, this, &PhotoSelectionDialog::onNext);
    controls->addWidget(next_btn_, 0, Qt::AlignRight | Qt::AlignVCenter);

    left_layout->addLayout(controls);

    // === 右侧：照片列表 ===
    auto* right_widget = new QWidget(this);
    right_widget->setMinimumWidth(320);
    right_widget->setMaximumWidth(400);
    right_widget->setStyleSheet(
        QStringLiteral("QWidget {"
        "  background: #0A0D12;"
        "  border: 2px solid #161D2B;"
        "  border-radius: 12px;"
        "}"));
    auto* right_layout = new QVBoxLayout(right_widget);
    right_layout->setContentsMargins(16, 16, 16, 16);
    right_layout->setSpacing(12);

    auto* list_title = new QLabel(QStringLiteral("已选照片"), right_widget);
    list_title->setStyleSheet(
        QStringLiteral("color: #E5E7EB;"
        "font-size: 16px;"
        "font-weight: 600;"
        "background: transparent;"
        "border: none;"));
    right_layout->addWidget(list_title);

    photo_list_scroll_ = new QScrollArea(right_widget);
    photo_list_scroll_->setWidgetResizable(true);
    photo_list_scroll_->setStyleSheet(
        QStringLiteral("QScrollArea {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QScrollBar:vertical {"
        "  background: #0F131C;"
        "  width: 8px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: #1E2636;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background: #38BDF8;"
        "}"));

    photo_list_container_ = new QWidget();
    photo_list_container_->setStyleSheet(QStringLiteral("background: transparent; border: none;"));
    photo_list_layout_ = new QVBoxLayout(photo_list_container_);
    photo_list_layout_->setContentsMargins(0, 0, 0, 0);
    photo_list_layout_->setSpacing(12);
    photo_list_layout_->addStretch();

    photo_list_scroll_->setWidget(photo_list_container_);
    right_layout->addWidget(photo_list_scroll_);

    // 添加到主布局
    main_layout->addWidget(left_widget, 3);
    main_layout->addWidget(right_widget, 1);

    // 快门音效
    shutter_sound_ = new QSoundEffect(this);
    shutter_sound_->setSource(QUrl(QStringLiteral("qrc:/shutter.wav")));
    shutter_sound_->setVolume(1.0);
}

void PhotoSelectionDialog::startCamera() {
    camera_decoder_ = new CameraPreviewDecoder(this);

    std::vector<PreviewDetectorConfig> preview_configs = detector_configs_;
    if (preview_configs.empty()) {
        const std::string& model_path = service_->getDetectionModelPath();
        if (!model_path.empty() && QFileInfo(QString::fromStdString(model_path)).isFile()) {
            preview_configs.push_back({PreviewDetectorConfig::Type::Face,
                                       model_path, {}, RKNN_NPU_CORE_0});
        } else {
            qWarning() << "SCRFD detection model is unavailable; camera preview will run without face detection";
        }
    }
    camera_decoder_->setDetectorConfigs(preview_configs);
    
    connect(camera_decoder_, &CameraPreviewDecoder::frameReady,
            camera_preview_, &GLVideoWidget::onFrameReady, Qt::DirectConnection);
    
    connect(camera_decoder_, &CameraPreviewDecoder::error, this,
            [this](const QString& message) {
                snap_btn_->setEnabled(true);
                qWarning() << "Camera error:" << message;
                QMessageBox::warning(this, QStringLiteral("摄像头"), message);
            });
    
    connect(camera_decoder_, &CameraPreviewDecoder::photoCaptured, this,
            &PhotoSelectionDialog::onPhotoCaptured, Qt::QueuedConnection);
    
    camera_decoder_->start(QStringLiteral("/dev/video41"));
}

void PhotoSelectionDialog::onTakePhoto() {
    if (!camera_decoder_) return;
    
    // 播放快门效果
    shutter_overlay_->setGeometry(camera_preview_->rect());
    shutter_overlay_->show();
    shutter_overlay_->raise();
    shutter_sound_->play();
    
    QTimer::singleShot(110, shutter_overlay_, [this]() {
        shutter_overlay_->hide();
    });
    
    // 生成照片路径
    Task task = service_->getTaskInfo(task_id_);
    const QString capture_base = QString::fromStdString(task.folder_path)
                               + QStringLiteral("/capture_")
                               + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    QString photo_path = capture_base + QStringLiteral(".jpg");
    int duplicate_index = 1;
    while (QFileInfo::exists(photo_path)) {
        photo_path = capture_base + QStringLiteral("_%1.jpg").arg(duplicate_index++);
    }
    
    snap_btn_->setEnabled(false);
    camera_decoder_->capture(photo_path);
}

void PhotoSelectionDialog::onPhotoCaptured(const QString& path, const QImage& image) {
    snap_btn_->setEnabled(true);
    
    if (!selected_photos_.contains(path)) {
        selected_photos_ << path;
    }
    
    // 更新底部缩略图
    thumbnail_label_->setPixmap(QPixmap::fromImage(image).scaled(
        thumbnail_label_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
    
    // 添加到右侧列表
    addPhotoToList(path, image);
    
    // 启用下一步按钮
    next_btn_->setEnabled(true);
}

void PhotoSelectionDialog::addPhotoToList(const QString& path, const QImage& image) {
    // 移除占位的stretch
    if (photo_list_layout_->count() > 0) {
        QLayoutItem* last = photo_list_layout_->itemAt(photo_list_layout_->count() - 1);
        if (last && dynamic_cast<QSpacerItem*>(last)) {
            photo_list_layout_->removeItem(last);
            delete last;
        }
    }
    
    // 创建照片卡片
    auto* card = new QWidget(photo_list_container_);
    card->setFixedSize(280, 210);
    card->setStyleSheet(
        QStringLiteral("QWidget {"
        "  background: #0F131C;"
        "  border: 2px solid #1E2636;"
        "  border-radius: 8px;"
        "}"));
    
    auto* card_layout = new QVBoxLayout(card);
    card_layout->setContentsMargins(0, 0, 0, 0);
    card_layout->setSpacing(0);
    
    // 图片
    auto* image_label = new QLabel(card);
    image_label->setFixedSize(280, 210);
    image_label->setAlignment(Qt::AlignCenter);
    
    QPixmap pixmap;
    if (!image.isNull()) {
        pixmap = QPixmap::fromImage(image);
    } else {
        pixmap = loadPixmapSafe(path);
    }
    
    if (!pixmap.isNull()) {
        image_label->setPixmap(pixmap.scaled(280, 210, Qt::KeepAspectRatio, Qt::SmoothTransformation));
    } else {
        image_label->setText(QStringLiteral("无法加载"));
        image_label->setStyleSheet(QStringLiteral("color: #DC2626; font-size: 14px;"));
    }
    
    card_layout->addWidget(image_label);
    
    // 删除按钮
    auto* delete_btn = new QPushButton(QStringLiteral("×"), card);
    delete_btn->setFixedSize(28, 28);
    delete_btn->move(280 - 36, 8);
    delete_btn->setCursor(Qt::PointingHandCursor);
    delete_btn->setStyleSheet(
        QStringLiteral("QPushButton {"
        "  background: rgba(220, 38, 38, 0.9);"
        "  color: white;"
        "  border: none;"
        "  border-radius: 14px;"
        "  font-size: 18px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover { background: #DC2626; }"));
    
    connect(delete_btn, &QPushButton::clicked, this, [this, path, card]() {
        selected_photos_.removeAll(path);
        card->deleteLater();
        next_btn_->setEnabled(!selected_photos_.isEmpty());
        
        // 如果列表为空，重新添加stretch
        if (selected_photos_.isEmpty()) {
            photo_list_layout_->addStretch();
        }
    });
    
    photo_list_layout_->addWidget(card);
    
    // 重新添加stretch到末尾
    photo_list_layout_->addStretch();
}

void PhotoSelectionDialog::onUploadPhotos() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, QStringLiteral("选择照片"), QString(),
        QStringLiteral("图像文件 (*.jpg *.jpeg *.png *.bmp)"));
    
    if (files.isEmpty()) return;
    
    for (const QString& file : files) {
        QFileInfo file_info(file);
        if (!file_info.exists() || !file_info.isFile()) continue;
        
        // 验证图片格式
        const QString suffix = file_info.suffix().toLower();
        if (suffix != QStringLiteral("jpg") && suffix != QStringLiteral("jpeg")) {
            QPixmap test_pixmap(file);
            if (test_pixmap.isNull()) continue;
        }
        
        if (!selected_photos_.contains(file)) {
            selected_photos_ << file;
            addPhotoToList(file, QImage());  // 延迟加载
        }
    }
    
    next_btn_->setEnabled(!selected_photos_.isEmpty());
}

void PhotoSelectionDialog::onNext() {
    if (selected_photos_.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("提示"), QStringLiteral("请先拍照或选择照片"));
        return;
    }
    
    const int open_fd_count = QDir(QStringLiteral("/proc/self/fd"))
        .entryList(QDir::AllEntries | QDir::NoDotAndDotDot).size();
        qDebug() << "Open file descriptors before photo recognition:" << open_fd_count;
    
    // 停止相机
    if (camera_decoder_) {
        camera_decoder_->stop();
    }
    
    emit photosConfirmed(selected_photos_);
    accept();
}

QPixmap PhotoSelectionDialog::loadPixmapSafe(const QString& path) {
    // 避免libjpeg版本冲突导致崩溃
    QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    
    Q_UNUSED(suffix);
    return ::loadPixmapSafe(path);
}

void PhotoSelectionDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (shutter_overlay_ && camera_preview_) {
        shutter_overlay_->setGeometry(camera_preview_->rect());
    }
}
