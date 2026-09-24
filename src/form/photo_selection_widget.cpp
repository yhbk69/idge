/*照片选择对话框（实现文件 photo_selection_widget.cpp，类定义见 photo_selection_widget.h）
两种照片来源：
拍照：
调用 /dev/video41 摄像头，通过 CameraPreviewDecoder 解码 MJPEG 流
实时预览，点击圆形快门按钮拍照
播放快门音效 + 白色闪屏动画
照片保存到任务文件夹，左下角显示缩略图
上传照片：文件选择器批量添加图片
删除：每张卡片右上角常驻 "×" 按钮（无独立编辑模式）
照片列表：右侧滚动区纵向排列，280×210 卡片
（历史版本为"网格每行3张360×360+编辑模式"，已由遗留文件
 photo_selection_dialog.h 承载，现版本不再使用该交互）
*/
// photo_selection_widget.cpp
#include "photo_selection_widget.h"
#include "theme.h"
#include "qt_image_utils.h"
#include "camera_preview_decoder.h"
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
// 圆形快门按钮：纯 QPainter 自绘（QSS 画不出正圆+粗圆环），
// 7px 圆环 theme::BORDER，内部填充按 禁用/按下/悬停/常态 四档灰阶变化；
// NoFocus 防止触摸屏点击后残留焦点框
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

        QColor fill = QColor(theme::TEXT);
        if (!isEnabled()) {
            fill = QColor(theme::TEXT);
        } else if (isDown()) {
            fill = QColor(theme::TEXT_MUTED);
        } else if (underMouse()) {
            fill = QColor(theme::TEXT);
        }

        painter.setPen(QPen(QColor(theme::BORDER), border_width));
        painter.setBrush(fill);
        painter.drawEllipse(circle);
    }
};
} // namespace

// 构造即建 UI 并启动相机：调用方以 exec() 模态使用，
// 相机生命周期完全包裹在对话框生命周期内（析构中 stop）
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
    // 顺序要求：先 stop() 让采集/解码线程退出并切断信号回调，
    // 再 deleteLater()（decoder 以 this 为 parent，也在对象树析构内），
    // 避免在途帧回调打到正在析构的 GL 控件
    if (camera_decoder_) {
        camera_decoder_->stop();
        camera_decoder_->deleteLater();
    }
}

// 左右分栏 3:1（主布局 addWidget stretch），右列固定 320~400px 放照片列表；
// 预览最小 960x540 保持 16:9 且给底部控制条留空间（屏高≈1080 的板端不裁按钮）；
// 快门遮罩是预览区的子 QLabel（随 resizeEvent 同步几何）；
// 整体 1600x900/最小 1400x800 按 RK3588 触屏 1080p 屏设计
void PhotoSelectionDialog::setupUI() {
    setWindowTitle(QStringLiteral("照片识别"));
    resize(1600, 900);
    setMinimumSize(1400, 800);
    setStyleSheet(QString("QWidget { background: %1; }").arg(theme::BG));

    auto* main_layout = new QHBoxLayout(this);
    main_layout->setContentsMargins(16, 16, 16, 16);
    main_layout->setSpacing(16);

    // === 左侧：相机预览区域 ===
    auto* left_widget = new QWidget(this);
    left_widget->setStyleSheet(QString("QWidget { background: %1; }").arg(theme::BG));
    auto* left_layout = new QVBoxLayout(left_widget);
    left_layout->setContentsMargins(0, 0, 0, 0);
    left_layout->setSpacing(16);

    // 顶部工具栏：只有"选择照片"按钮
    auto* toolbar = new QWidget(left_widget);
    auto* toolbar_layout = new QHBoxLayout(toolbar);
    toolbar_layout->setContentsMargins(0, 0, 0, 0);

    QString button_style = 
        QString("QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 10px 24px;"
        "  font-size:%3px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: %4; }"
        "QPushButton:pressed { background: %5; }")
        .arg(theme::ACCENT, theme::BG)
        .arg(theme::FS_CARD)
        .arg(theme::SKY, theme::SKY_PRESSED);

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
        QString("background: %1;"
        "border: 2px solid %2;"
        "border-radius: 12px;")
        .arg(theme::FIELD, theme::BORDER));
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
        QString("background: #000000;"
        "border: 2px solid %1;"
        "border-radius: 8px;")
        .arg(theme::BORDER));
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
        QString("QPushButton {"
        "  background: %1;"
        "  color: %2;"
        "  border: none;"
        "  border-radius: 12px;"
        "  padding: 10px 32px;"
        "  font-size:%3px;"
        "  font-weight: 700;"
        "}"
        "QPushButton:hover { background: %4; }"
        "QPushButton:disabled { background: %5; color: %6; }")
        .arg(theme::SUCCESS, theme::BG)
        .arg(theme::FS_CARD)
        .arg(theme::SUCCESS_HOVER, theme::PANEL, theme::TEXT_MUTED));
    connect(next_btn_, &QPushButton::clicked, this, &PhotoSelectionDialog::onNext);
    controls->addWidget(next_btn_, 0, Qt::AlignRight | Qt::AlignVCenter);

    left_layout->addLayout(controls);

    // === 右侧：照片列表 ===
    auto* right_widget = new QWidget(this);
    right_widget->setMinimumWidth(320);
    right_widget->setMaximumWidth(400);
    right_widget->setStyleSheet(
        QString("QWidget {"
        "  background: %1;"
        "  border: 2px solid %2;"
        "  border-radius: 12px;"
        "}")
        .arg(theme::FIELD, theme::BORDER));
    auto* right_layout = new QVBoxLayout(right_widget);
    right_layout->setContentsMargins(16, 16, 16, 16);
    right_layout->setSpacing(12);

    auto* list_title = new QLabel(QStringLiteral("已选照片"), right_widget);
    list_title->setStyleSheet(
        QString("color: %1;"
        "font-size:%2px;"
        "font-weight: 600;"
        "background: transparent;"
        "border: none;")
        .arg(theme::TEXT)
        .arg(theme::FS_CARD));
    right_layout->addWidget(list_title);

    photo_list_scroll_ = new QScrollArea(right_widget);
    photo_list_scroll_->setWidgetResizable(true);
    photo_list_scroll_->setStyleSheet(
        QString("QScrollArea {"
        "  background: transparent;"
        "  border: none;"
        "}"
        "QScrollBar:vertical {"
        "  background: %1;"
        "  width: 8px;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %2;"
        "  border-radius: 4px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background: %3;"
        "}")
        .arg(theme::PANEL, theme::HOVER, theme::ACCENT));

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

/**
 * @brief 组装并启动相机预览管线
 *
 * 检测器兜底策略：调用方未传 detector_configs 时取服务的人脸模型
 * （绑定 RKNN_NPU_CORE_0，与主检测流水线的核分配约定一致）；模型文件
 * 缺失则空配置启动，仅纯预览不画框，功能降级但流程可用。
 *
 * 连接类型（线程安全关键点）：
 *   - frameReady → GLVideoWidget::onFrameReady：Qt::DirectConnection，
 *     在解码线程内执行"加锁换帧 + update()"，不触碰 GL 上下文（paintGL
 *     仍在 GUI 线程），改回 AutoConnection 会引入每帧队列投递开销；
 *   - error → lambda：未显式指定，跨线程时按 Auto 排队到 GUI 线程弹窗
 *     （QMessageBox 只能在 GUI 线程创建）；回调里恢复快门按钮可用性；
 *   - photoCaptured → onPhotoCaptured：显式 Qt::QueuedConnection，
 *     落盘在解码线程完成、UI 更新回到 GUI 线程，QImage 按值拷贝跨线程安全。
 * 设备路径 /dev/video41 为板端固定 USB 摄像头节点（硬编码，换硬件需改）。
 */
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

// 快门动作：白色遮罩闪 110ms（singleShot 定时器，视觉上模拟单反闪屏）+
// 音效；照片名 capture_yyyyMMdd_HHmmss_zzz.jpg 毫秒级时间戳仍可能同帧撞名，
// 故用 while 递增 _1/_2... 后缀保证任务目录内不覆盖；
// 期间禁用快门防连点重入，真正回调解码线程落盘后经队列连接在 onPhotoCaptured 恢复
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

// 解码线程落盘完成后经 Qt::QueuedConnection 回到 GUI 线程：
// 入列表（contains 去重）、刷新 128x92 缩略图、启用"下一步"、恢复快门
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

// 右侧列表追加一张 280x210 卡片（图占满卡片，× 按钮以绝对坐标
// move(280-36, 8) 钉在右上角，radius 14 → 直径 28 的圆钮）。
// image 为空表示上传的本地照片：不在此处解码，显示时才 loadPixmapSafe
// （延迟加载，避免批量上传大图阻塞 UI）。
// 布局末尾的 stretch 占位：插入前移除、插入后补回，保证卡片顶部对齐。
// 删除按钮 lambda 按值捕获 path/card：card->deleteLater() 后若列表已空
// 重新补 stretch 并联动"下一步"可用性。
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
        QString("QWidget {"
        "  background: %1;"
        "  border: 2px solid %2;"
        "  border-radius: 8px;"
        "}")
        .arg(theme::PANEL, theme::HOVER));
    
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
        image_label->setStyleSheet(theme::text(theme::DANGER, theme::FS_BODY));
    }
    
    card_layout->addWidget(image_label);
    
    // 删除按钮
    auto* delete_btn = new QPushButton(QStringLiteral("×"), card);
    delete_btn->setFixedSize(28, 28);
    delete_btn->move(280 - 36, 8);
    delete_btn->setCursor(Qt::PointingHandCursor);
    delete_btn->setStyleSheet(
        QString("QPushButton {"
        "  background: rgba(220, 38, 38, 0.9);"
        "  color: white;"
        "  border: none;"
        "  border-radius: 14px;"
        "  font-size:%1px;"
        "  font-weight: bold;"
        "}"
        "QPushButton:hover { background: %2; }")
        .arg(theme::FS_CARD)
        .arg(theme::DANGER));
    
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

// 批量上传：jpg/jpeg 直接信任后缀，png/bmp 等其他后缀先用 QPixmap 试解码
// 过滤坏图；contains 去重后以空 QImage 延迟加载加入列表
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

// "下一步"出口：先打印 /proc/self/fd 打开句柄数（识别流程会大量开文件，
// 该 qDebug 是上一轮"fd 泄漏卡死"排查留下的观测点，非业务逻辑），
// 然后必须在 accept 前 stop() 相机——否则采集线程继续回调即将析构的界面；
// photosConfirmed 信号当前无接收者，实际数据靠调用方 getSelectedPhotos() 取回
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

// 成员版仅是全局 qt_image_utils 同名函数的薄包装（历史版本按后缀分流解码，
// 现统一交给全局实现；suffix/QFileInfo 取值为遗留无效计算，勿在此加逻辑）
QPixmap PhotoSelectionDialog::loadPixmapSafe(const QString& path) {
    // 避免libjpeg版本冲突导致崩溃
    QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    
    Q_UNUSED(suffix);
    return ::loadPixmapSafe(path);
}

// 预览区随窗口缩放时，闪白遮罩必须同步几何，否则闪光覆盖区错位
void PhotoSelectionDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    if (shutter_overlay_ && camera_preview_) {
        shutter_overlay_->setGeometry(camera_preview_->rect());
    }
}
