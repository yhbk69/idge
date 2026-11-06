// ============================================================================
// capture_dialog.cpp - 处置取证拍照框
// ============================================================================
//
// 【所属模块】src/ui/widgets。与点名/盘点的 PhotoSelectionDialog 共用同一套
//   相机原语（CameraPreviewDecoder + GLVideoWidget），差别只有两处，且都是
//   2026-11-06 现场指定的：
//     1. **没有"选择照片"**：整改照片只能现拍，从相册挑旧图等于伪造到场证据；
//     2. **不挂检测模型**：detector configs 传空 = 纯预览。四路检测已经把三个
//        NPU 核占满，这里再起一个 SCRFD 只会拖慢主链路，而取证要的是那张图本身。
// 【抓拍为什么是原始码流】CameraPreviewDecoder::capture() 对 MJPEG 源直接落盘
//   缓存的 JPEG 码流（不重编码），对 NV12/YUYV 源现场编码当前帧。副作用与
//   PhotoSelectionDialog 相同：抓拍图**不带预览叠加的检测框**，这里也不想要——
//   处置照片是给人核对现场的，框会挡人。
// 【线程】frameReady→onFrameReady 用 DirectConnection（解码线程内换帧 + update()，
//   真正的 GL 绘制仍在 GUI 线程 paintGL）；photoCaptured 用 QueuedConnection，
//   落盘在解码线程完成、UI 更新回 GUI 线程。改回 Auto 会给每帧加一次队列投递。
// 【析构顺序】先 decoder_->stop() 切断在途回调，再 deleteLater()——反过来就是
//   回调打到正在析构的 GL 控件上（本仓已有同一教训，见 photo_selection_widget.cpp）。
// ============================================================================
#include "capture_dialog.h"

#include "ConfigManager.h"
#include "camera_preview_decoder.h"
#include "gl_video_widget.h"
#include "theme.h"

#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kThumbW = 150;
constexpr int kThumbH = 106;

QString buttonQss(const char *bg, const char *hover)
{
    return QString("QPushButton { color: %1; background: %2; border: none;"
                   " border-radius: 8px; padding: 10px 22px; font-size:%3px; font-weight:600; }"
                   "QPushButton:hover { background: %4; }"
                   "QPushButton:disabled { background: %5; color: %6; }")
        .arg(theme::BG, bg)
        .arg(theme::FS_BODY)
        .arg(hover, theme::PANEL, theme::TEXT_MUTED);
}
}  // namespace

CaptureDialog::CaptureDialog(const QString &dir, int maxCount, QWidget *parent)
    : QDialog(parent), dir_(dir), maxCount_(maxCount < 1 ? 1 : maxCount)
{
    setupUi();
    // 目录先建好：快门按下去才发现目录不存在，用户看到的就是"拍了没反应"。
    if (!QDir().mkpath(dir_))
        qWarning() << "[CaptureDialog] 处置照片目录建不出来:" << dir_;
    startCamera();
}

CaptureDialog::~CaptureDialog()
{
    if (decoder_) {
        decoder_->stop();
        decoder_->deleteLater();
    }
}

void CaptureDialog::setupUi()
{
    setWindowTitle(QString::fromUtf8("处置拍照"));
    setStyleSheet(QString("QDialog { background: %1; }").arg(theme::BG));
    setMinimumSize(860, 640);
    resize(1000, 700);

    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    preview_ = new GLVideoWidget(this);
    preview_->setMinimumSize(640, 360);
    preview_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    preview_->setStyleSheet(QString("background: %1; border: 2px solid %2; border-radius: 12px;")
                                .arg(theme::FIELD, theme::BORDER));
    root->addWidget(preview_, 1);

    // 闪白遮罩是预览区的子控件：放在 preview_ 下面才能盖住画面（与点名流程同一做法）
    shutterOverlay_ = new QLabel(preview_);
    shutterOverlay_->setGeometry(preview_->rect());
    shutterOverlay_->setStyleSheet(QStringLiteral("background-color: rgba(255, 255, 255, 235);"));
    shutterOverlay_->hide();

    thumbArea_ = new QWidget(this);
    thumbLayout_ = new QHBoxLayout(thumbArea_);
    thumbLayout_->setContentsMargins(0, 0, 0, 0);
    thumbLayout_->setSpacing(10);
    thumbLayout_->addStretch();
    root->addWidget(thumbArea_);

    hintLabel_ = new QLabel(QString::fromUtf8("已拍 0/%1 张").arg(maxCount_), this);
    hintLabel_->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_HINT));
    root->addWidget(hintLabel_);

    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(12);

    snapButton_ = new QPushButton(QString::fromUtf8("拍照"), this);
    snapButton_->setMinimumSize(120, 48);
    snapButton_->setStyleSheet(buttonQss(theme::ACCENT, theme::SKY));
    connect(snapButton_, &QPushButton::clicked, this, &CaptureDialog::onTakePhoto);
    btnRow->addWidget(snapButton_);

    btnRow->addStretch();

    okButton_ = new QPushButton(QString::fromUtf8("用这些照片"), this);
    okButton_->setMinimumSize(140, 48);
    okButton_->setStyleSheet(buttonQss(theme::SUCCESS, theme::SUCCESS_HOVER));
    // 一张都没拍就点"用这些照片"没有意义（处置可以不带照片，但那要用户在处置框里
    // 主动选"不拍照"，而不是在这个已经打开的相机里点确定）。
    okButton_->setEnabled(false);
    connect(okButton_, &QPushButton::clicked, this, [this]() {
        accepted_ = true;   // 这之后照片归调用方所有，不再由本框回收
        accept();
    });
    btnRow->addWidget(okButton_);

    auto *btnCancel = new QPushButton(QString::fromUtf8("取消"), this);
    btnCancel->setMinimumSize(100, 48);
    btnCancel->setStyleSheet(buttonQss(theme::HOVER, theme::HOVER));
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnRow->addWidget(btnCancel);

    root->addLayout(btnRow);
}

void CaptureDialog::startCamera()
{
    decoder_ = new CameraPreviewDecoder(this);
    // 空配置 = 纯预览不跑检测，理由见文件头
    decoder_->setDetectorConfigs({});

    connect(decoder_, &CameraPreviewDecoder::frameReady,
            preview_, &GLVideoWidget::onFrameReady, Qt::DirectConnection);
    connect(decoder_, &CameraPreviewDecoder::error, this,
            &CaptureDialog::onCameraError);
    connect(decoder_, &CameraPreviewDecoder::photoCaptured,
            this, &CaptureDialog::onCaptured, Qt::QueuedConnection);

    decoder_->start(ConfigManager::instance().smartRecognitionCamera());
}

// 快门：闪白 110ms + 禁按钮防连点，真正落盘在解码线程完成后经回调恢复。
void CaptureDialog::onTakePhoto()
{
    if (!decoder_ || photos_.size() >= maxCount_)
        return;

    shutterOverlay_->setGeometry(preview_->rect());
    shutterOverlay_->show();
    shutterOverlay_->raise();
    QTimer::singleShot(110, shutterOverlay_, [this]() { shutterOverlay_->hide(); });

    snapButton_->setEnabled(false);
    decoder_->capture(nextFilePath());
}

QString CaptureDialog::nextFilePath() const
{
    // dispose1.jpg / dispose2.jpg …与 Pad 提交的那批同名同序，
    // 从盘上现有文件往后数（不是从 photos_.size() 数）：同一报警可能分几次进这个框，
    // 从 1 重来会把上一次已经记进库的那张覆盖掉。
    const QStringList existing = QDir(dir_).entryList(
        {QStringLiteral("dispose*.jpg")}, QDir::Files, QDir::Unsorted);
    int index = existing.size() + 1;
    QString file;
    do {
        file = QDir(dir_).filePath(QStringLiteral("dispose%1.jpg").arg(index));
        ++index;
    } while (QFileInfo::exists(file));
    return file;
}

void CaptureDialog::onCaptured(const QString &path, const QImage &thumbnail)
{
    snapButton_->setEnabled(photos_.size() < maxCount_);
    if (!QFileInfo::exists(path)) {
        // 解码线程说"拍完了"但盘上没有：capture() 的两种落盘路径都可能在写失败时
        // 照样发信号（无 JPEG 码流且编码失败），这里必须说出来而不是静默少一张。
        qWarning() << "[CaptureDialog] 抓拍文件不在:" << path;
        return;
    }
    if (!photos_.contains(path))
        photos_ << path;
    if (!takenFiles_.contains(path))
        takenFiles_ << path;

    // 缩略图条：一张卡片 = 图 + 下面一个"删"字按钮（触屏 44px 高，够用且不挡图）
    auto *card = new QWidget(thumbArea_);
    auto *cardLay = new QVBoxLayout(card);
    cardLay->setContentsMargins(0, 0, 0, 0);
    cardLay->setSpacing(4);

    auto *pic = new QLabel(card);
    pic->setFixedSize(kThumbW, kThumbH);
    pic->setAlignment(Qt::AlignCenter);
    pic->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 6px;")
                           .arg(theme::FIELD, theme::BORDER));
    pic->setPixmap(QPixmap::fromImage(thumbnail).scaled(pic->size(), Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation));
    cardLay->addWidget(pic);

    auto *del = new QPushButton(QString::fromUtf8("删除"), card);
    del->setMinimumHeight(36);
    del->setStyleSheet(QString("QPushButton { color: %1; background: %2; border: none;"
                               " border-radius: 6px; font-size:%3px; }")
                           .arg(theme::DANGER, theme::PANEL)
                           .arg(theme::FS_HINT));
    const QString thisPath = path;
    connect(del, &QPushButton::clicked, this, [this, thisPath, card]() {
        photos_.removeAll(thisPath);
        // 文件当场删掉：这张是本框刚拍的，还没进任何一条处置，留着就是孤儿。
        // 不删的话下一次进来 disposeN 会继续往后编，目录里堆的是一堆没人看的 jpg。
        QFile::remove(thisPath);
        takenFiles_.removeAll(thisPath);
        card->deleteLater();
        hintLabel_->setText(QString::fromUtf8("已拍 %1/%2 张").arg(photos_.size()).arg(maxCount_));
        snapButton_->setEnabled(photos_.size() < maxCount_);
        okButton_->setEnabled(!photos_.isEmpty());
    });
    cardLay->addWidget(del);

    // 插在末尾 stretch 之前，卡片才能顶对齐排下去（与点名那个框同一手法）
    thumbLayout_->insertWidget(thumbLayout_->count() - 1, card);

    hintLabel_->setText(QString::fromUtf8("已拍 %1/%2 张").arg(photos_.size()).arg(maxCount_));
    okButton_->setEnabled(!photos_.isEmpty());
    if (photos_.size() >= maxCount_)
        snapButton_->setEnabled(false);
}

void CaptureDialog::onCameraError(const QString &message)
{
    snapButton_->setEnabled(true);
    qWarning() << "[CaptureDialog] 摄像头:" << message;
    QMessageBox::warning(this, QString::fromUtf8("摄像头"), message);
}

void CaptureDialog::discardIfNotAccepted()
{
    if (accepted_)
        return;
    for (const QString &abs : takenFiles_)
        QFile::remove(abs);
    takenFiles_.clear();
    photos_.clear();
}

void CaptureDialog::reject()
{
    discardIfNotAccepted();
    QDialog::reject();
}

// 窗口 ✕ 那条路不走 reject()，接了才有同一份收口。
void CaptureDialog::closeEvent(QCloseEvent *event)
{
    discardIfNotAccepted();
    QDialog::closeEvent(event);
}
