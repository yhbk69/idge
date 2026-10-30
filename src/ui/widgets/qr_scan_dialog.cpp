#include "qr_scan_dialog.h"

#include "capture_button.h"
#include "gl_video_widget.h"
#include "qr_decoder.h"
#include "theme.h"
#include "ConfigManager.h"
#include "ffmpeg_video_decoder.h"

#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include <vector>

// 从解码器映射指针获取 RGBA 帧，转为 QImage（RGB888）供显示与解码
QImage captureFrameAsImage(FFmpegVideoDecoder *decoder)
{
    const uint8_t *ptr = decoder->captureCurrentFramePtr();
    if (!ptr)
        return {};
    int w = 0, h = 0, stride = 0;
    if (!decoder->getCurrentFrameInfo(w, h, stride))
        return {};

    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        const uint8_t *srcRow = ptr + static_cast<size_t>(y) * stride;
        uint8_t *dstRow = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
    }
    return img;
}

namespace {

using uiwidgets::CaptureButton;

// 多码标记：箭头按钮（只在结果框的冻结图叠层上用）
class ArrowButton : public QPushButton {
public:
    explicit ArrowButton(QWidget *parent = nullptr) : QPushButton(parent) {
        setFixedSize(72, 72);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(QString(
            "QPushButton { background: %1; border: 5px solid white; border-radius: 36px; }"
            "QPushButton:hover { background: %2; }")
            .arg(theme::SUCCESS, theme::SUCCESS_HOVER));
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPushButton::paintEvent(nullptr);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const int cx = width() / 2;
        const int cy = height() / 2;
        painter.drawLine(cx - 12, cy - 14, cx + 10, cy);
        painter.drawLine(cx + 10, cy, cx - 12, cy + 14);
    }
};

// 模态预览：开一台 PreviewOnly 解码器，用户点快门取一帧。
// 返回 null QImage = 用户没拍照就关掉了窗口（不是错误）。
// 解码器在本函数返回前一定停掉并释放，这样"退出预览后另一个页面立刻能开同一设备节点"成立。
QImage previewAndCapture(QWidget *parent)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QString::fromUtf8("二维码识别 - 拍照"));
    dialog.resize(960, 640);
    dialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *previewContainer = new QWidget(&dialog);
    previewContainer->setStyleSheet(QStringLiteral("background:#000000;"));
    auto *previewLayout = new QVBoxLayout(previewContainer);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    auto *previewWidget = new GLVideoWidget(previewContainer);
    previewWidget->setMinimumSize(960, 540);
    previewLayout->addWidget(previewWidget);

    root->addWidget(previewContainer, 1);

    auto *captureBtn = new CaptureButton(previewWidget);
    auto positionCapture = [captureBtn, previewWidget]() {
        captureBtn->move((previewWidget->width() - captureBtn->width()) / 2,
                         previewWidget->height() - captureBtn->height() - 24);
        captureBtn->raise();
    };
    positionCapture();
    QTimer::singleShot(0, positionCapture);
    captureBtn->setToolTip(QString::fromUtf8("拍照识别"));

    const QString cameraUrl = ConfigManager::instance().smartRecognitionCamera();
    // 只取画面帧给 ZBar，不参与级联推理：PreviewOnly 不加载任何 RKNN 模型
    auto *decoder = new FFmpegVideoDecoder(
        nullptr, FFmpegVideoDecoder::InferenceMode::PreviewOnly);
    QObject::connect(decoder, &FFmpegVideoDecoder::frameReady,
                     previewWidget, &GLVideoWidget::onFrameReady, Qt::DirectConnection);
    decoder->start(cameraUrl);

    QImage capturedImage;

    QObject::connect(captureBtn, &QPushButton::clicked, [&dialog, decoder, &capturedImage]() {
        capturedImage = captureFrameAsImage(decoder);
        if (capturedImage.isNull()) {
            QMessageBox::warning(&dialog, QString::fromUtf8("错误"),
                                 QString::fromUtf8("无法获取相机画面"));
            return;
        }
        decoder->stop();
        dialog.accept();
    });

    dialog.exec();
    decoder->stop();
    delete decoder;

    // 只有快门按钮会 accept()，而 accept 的前提是帧非空（取不到帧时在框内就地提示、
    // 窗口不关）。所以这里返回 null 只代表"用户自己关掉了预览"，登录页不该当故障报。
    return capturedImage;
}

// 结果框：冻结图打底，单码直接弹内容，多码在码位上叠箭头按钮供点选。
void showQrResults(QWidget *parent, const QImage &frame,
                   const std::vector<qrcode::QrHit> &hits)
{
    QDialog resultDialog(parent);
    resultDialog.setWindowTitle(QString::fromUtf8("二维码识别结果"));
    resultDialog.resize(960, 640);
    resultDialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto *resultRoot = new QVBoxLayout(&resultDialog);
    resultRoot->setContentsMargins(0, 0, 0, 0);
    resultRoot->setSpacing(0);

    auto *resultPreviewContainer = new QWidget(&resultDialog);
    resultPreviewContainer->setStyleSheet(QStringLiteral("background:#000000;"));
    auto *resultPreviewLayout = new QVBoxLayout(resultPreviewContainer);
    resultPreviewLayout->setContentsMargins(0, 0, 0, 0);
    resultPreviewLayout->setSpacing(0);

    auto *frozenImageLabel = new QLabel(resultPreviewContainer);
    frozenImageLabel->setMinimumSize(960, 540);
    frozenImageLabel->setAlignment(Qt::AlignCenter);
    resultPreviewLayout->addWidget(frozenImageLabel);

    resultRoot->addWidget(resultPreviewContainer, 1);

    auto *hintLabel = new QLabel(&resultDialog);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setStyleSheet(QString(
        "color:%1; font-size:%2px; font-weight:500; background:%3; padding:14px;"
        "border-top:2px solid %4;")
        .arg(theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::PANEL, theme::BORDER));
    hintLabel->hide();
    resultRoot->addWidget(hintLabel);

    auto *closeBtn = new QPushButton(QString::fromUtf8("关闭"), &resultDialog);
    closeBtn->setMinimumHeight(50);
    closeBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:%2; border:none; font-size:%3px; font-weight:600; }"
        "QPushButton:hover { background:%4; } QPushButton:pressed { background:%5; }")
        .arg(theme::HOVER, theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::BORDER, theme::PANEL));
    resultRoot->addWidget(closeBtn);
    QObject::connect(closeBtn, &QPushButton::clicked, &resultDialog, &QDialog::reject);

    QPixmap pixmap = QPixmap::fromImage(frame);
    const auto scaledPixmap = pixmap.scaled(resultPreviewContainer->minimumSize(),
                                            Qt::KeepAspectRatio, Qt::SmoothTransformation);
    frozenImageLabel->setPixmap(scaledPixmap);

    resultDialog.show();
    QApplication::processEvents();

    const QSize labelSize = frozenImageLabel->size();
    const double scaleX = static_cast<double>(scaledPixmap.width()) / frame.width();
    const double scaleY = static_cast<double>(scaledPixmap.height()) / frame.height();

    if (hits.size() == 1) {
        QMessageBox msgBox(&resultDialog);
        msgBox.setWindowTitle(QString::fromUtf8("识别结果"));
        msgBox.setText(hits[0].content);
        msgBox.setStandardButtons(QMessageBox::Ok);
        msgBox.exec();
    } else {
        auto *overlayWidget = new QWidget(resultPreviewContainer);
        overlayWidget->setGeometry(0, 0, labelSize.width(), labelSize.height());
        overlayWidget->setStyleSheet(QStringLiteral("background:rgba(0, 0, 0, 0.5);"));
        overlayWidget->raise();
        overlayWidget->show();

        hintLabel->setText(QString::fromUtf8("识别到 %1 个二维码，请点击查看")
                               .arg(static_cast<int>(hits.size())));
        hintLabel->show();

        for (const auto &hit : hits) {
            const int centerX = static_cast<int>((hit.rect.x() + hit.rect.width() / 2.0) * scaleX);
            const int centerY = static_cast<int>((hit.rect.y() + hit.rect.height() / 2.0) * scaleY);

            auto *arrowBtn = new ArrowButton(overlayWidget);
            arrowBtn->move(centerX - 36, centerY - 36);
            arrowBtn->show();

            QObject::connect(arrowBtn, &QPushButton::clicked, [&resultDialog, hit]() {
                QMessageBox msgBox(&resultDialog);
                msgBox.setWindowTitle(QString::fromUtf8("识别结果"));
                msgBox.setText(hit.content);
                msgBox.setStandardButtons(QMessageBox::Ok);
                msgBox.exec();
            });
        }
    }

    resultDialog.exec();
}

} // namespace

bool QrScanDialog::available()
{
    return qrcode::decodeAvailable();
}

bool QrScanDialog::captureFrame(QWidget *parent, QImage *frame, QString *err)
{
    if (!frame)
        return false;
    if (!qrcode::decodeAvailable()) {
        // 没有 ZBar 时连预览都不该开：开了也解不出码，白占摄像头
        if (err) *err = qrcode::unavailableReason();
        return false;
    }
    const QImage captured = previewAndCapture(parent);
    if (captured.isNull())
        return false;   // 取消，不填 err
    *frame = captured;
    return true;
}

bool QrScanDialog::scan(QWidget *parent, QString *content, QString *err)
{
    QImage frame;
    if (!captureFrame(parent, &frame, err))
        return false;

    std::vector<qrcode::QrHit> hits;
    if (!qrcode::scanQrFromImage(frame, &hits, err))
        return false;
    if (hits.empty()) {
        if (err) *err = QString::fromUtf8("未检测到二维码，请把码对准取景框中央后重新拍照");
        return false;
    }
    if (content) *content = hits.front().content;
    return true;
}

void QrScanDialog::openFullScanner(QWidget *parent)
{
    if (!qrcode::decodeAvailable()) {
        QMessageBox::information(parent, QString::fromUtf8("不可用"), qrcode::unavailableReason());
        return;
    }

    QImage frame;
    QString error;
    if (!captureFrame(parent, &frame, &error)) {
        // err 为空 = 用户自己关掉了预览，不是故障，不打扰
        if (!error.isEmpty())
            QMessageBox::warning(parent, QString::fromUtf8("错误"), error);
        return;
    }

    std::vector<qrcode::QrHit> hits;
    if (!qrcode::scanQrFromImage(frame, &hits, &error)) {
        QMessageBox::warning(parent, QString::fromUtf8("错误"), error);
        return;
    }
    if (hits.empty()) {
        QMessageBox::information(parent, QString::fromUtf8("识别结果"),
                                 QString::fromUtf8("未检测到二维码，请重新拍照"));
        return;
    }

    showQrResults(parent, frame, hits);
}
