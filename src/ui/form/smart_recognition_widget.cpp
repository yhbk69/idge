#pragma execution_character_set("utf-8")

/**
 * @file smart_recognition_widget.cpp
 * @brief 智能识别页实现 —— 二维码 + OCR 铭牌 + 人员点名 + 设备盘点（四卡片入口）
 *
 * 页面本身只是一张四卡片的入口页（setupUi）。前两张是识别工具：
 *   1. 二维码：本文件只剩一行 `QrScanDialog::openFullScanner(this)`。
 *      预览、实时扫描（窗格打开即扫，没有快门键）、ZBar 解码、结果区（单码直显内容、
 *      多码在冻结图上叠圆点按钮点选）都在共用模块 src/ui/widgets/qr_scan_dialog.cpp 里
 *      —— **登录页扫码调的是同一个模块的 scan()**，因为摄像头设备节点不能同时被两处打开，
 *      两处也不该解出不同结果。用户点「确认」才关窗格。
 *      解码本身在 src/biz/qrcode/qr_decoder.cpp（无 GUI 依赖，可离线自测）。
 *   2. OCR：openOcrScanner() → showOcrPhotoDialog()（拍照存临时 jpg）→
 *      showOcrRegionDialog()（跑 det 拿文本框、smartGroupBoxes() 合并成可选区、
 *      用户可拖拽调框）→ showOcrResultDialog()（跑 system 输出 CTC 解码文本）。
 *      两阶段都不在本进程内推理，而是 QProcess 起 3rdparty/ppocrv4/bin 的
 *      rknn_model_zoo 官方 demo 并解析 stdout，详见 3rdparty/ppocrv4/README.md。
 * 后两张是 caichao 业务页入口，点击只 emit rollCallRequested/equipmentRequested，
 * 真正的任务列表页与模态弹窗由 frmMain 负责拉起（服务与页面所有权都在那边）。
 *
 * 三条容易踩的前提：
 *   - 二维码能力取决于编译期有没有 HAS_ZBAR（cmake 找到 3rdparty/zbar 才定义），
 *     没有则该卡片直接禁用，页面其余部分照常。
 *   - 两个对话框各开一台解码器，取流地址都读 config.json 的
 *     smartRecognition.camera；**同一设备节点不能被开两次**，所以扫码期间别同时
 *     再开登录页的扫码框（两者都是模态框，天然不会重叠）。
 *   - 二维码识别在拍照后对单帧做 ZBar 解码，不再有实时扫描线程，
 *     因此无需线程同步与稳定性检测。
 *
 * 本文件内有多个 Q_OBJECT 类定义在匿名 namespace，故末尾必须
 * #include "smart_recognition_widget.moc"，漏掉会连不过。
 */

#include "smart_recognition_widget.h"
#include "capture_button.h"        // uiwidgets::CaptureButton：圆形快门，现在只有 OCR 拍照流程在用
#include "ffmpeg_video_decoder.h"
#include "gl_video_widget.h"
#include "qr_scan_dialog.h"        // 共用扫码流程（登录页用的是同一套）
#include "theme.h"
#include "qthelper.h"   // enableTouchScroll：按需 new 的对话框扫描不到，要在创建处显式挂
#include "ConfigManager.h"

#include <QApplication>
#include <QDialog>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsScene>
#include <QGraphicsView>
#include <QGraphicsRectItem>
#include <QGraphicsPixmapItem>
#include <QGraphicsSceneMouseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScrollArea>
#include <QTemporaryFile>
#include <QTimer>
#include <QVBoxLayout>
#include <QRegExp>
#include <QPainter>
#include <QMouseEvent>
#include <QImage>

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cstdio>
#include <limits>

namespace {

void ocrTrace(const QString& step) {
    const QByteArray line = QStringLiteral("[OCR] %1 %2\n")
                                .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs), step)
                                .toUtf8();
    std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stderr);
    std::fflush(stderr);
}

QLabel* textLabel(const QString& text, int size, const char* color, QWidget* parent,
                  bool bold = false) {
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setStyleSheet(theme::text(color, size, bold));
    return label;
}

// ============================================================================
// UI 辅助控件
// ============================================================================

// OCR 区域选择：可拖拽调整大小的矩形
class ResizableRectItem : public QGraphicsRectItem {
public:
    explicit ResizableRectItem(const QRectF& rect, QGraphicsItem* parent = nullptr)
        : QGraphicsRectItem(QRectF(0, 0, rect.width(), rect.height()), parent),
          resizing_(false), edges_(Qt::Edges()), pressPos_(), pressRect_(), pressItemPos_() {
        setPos(rect.topLeft());
        setFlags(QGraphicsItem::ItemIsMovable | QGraphicsItem::ItemIsSelectable);
        setAcceptHoverEvents(true);
    }

protected:
    void hoverMoveEvent(QGraphicsSceneHoverEvent* event) override {
        const Qt::Edges edges = hitEdges(event->pos());
        if (edges == (Qt::TopEdge | Qt::LeftEdge) || edges == (Qt::BottomEdge | Qt::RightEdge))
            setCursor(Qt::SizeFDiagCursor);
        else if (edges == (Qt::TopEdge | Qt::RightEdge) || edges == (Qt::BottomEdge | Qt::LeftEdge))
            setCursor(Qt::SizeBDiagCursor);
        else if (edges & (Qt::LeftEdge | Qt::RightEdge))
            setCursor(Qt::SizeHorCursor);
        else if (edges & (Qt::TopEdge | Qt::BottomEdge))
            setCursor(Qt::SizeVerCursor);
        else
            setCursor(Qt::SizeAllCursor);
        QGraphicsRectItem::hoverMoveEvent(event);
    }

    void mousePressEvent(QGraphicsSceneMouseEvent* event) override {
        edges_ = hitEdges(event->pos());
        resizing_ = edges_ != Qt::Edges();
        pressPos_ = event->scenePos();
        pressRect_ = rect();
        pressItemPos_ = pos();
        QGraphicsRectItem::mousePressEvent(event);
    }

    void mouseMoveEvent(QGraphicsSceneMouseEvent* event) override {
        if (!resizing_) {
            QGraphicsRectItem::mouseMoveEvent(event);
            return;
        }

        const QPointF delta = event->scenePos() - pressPos_;
        QRectF next = pressRect_;
        QPointF nextPos = pressItemPos_;
        constexpr qreal minSize = 20.0;

        if (edges_ & Qt::LeftEdge) {
            const qreal left = qMin(pressRect_.right() - minSize, pressRect_.left() + delta.x());
            next.setLeft(left);
            nextPos.setX(pressItemPos_.x() + (left - pressRect_.left()));
            next.moveLeft(0);
        }
        if (edges_ & Qt::RightEdge)
            next.setRight(qMax(pressRect_.left() + minSize, pressRect_.right() + delta.x()));
        if (edges_ & Qt::TopEdge) {
            const qreal top = qMin(pressRect_.bottom() - minSize, pressRect_.top() + delta.y());
            next.setTop(top);
            nextPos.setY(pressItemPos_.y() + (top - pressRect_.top()));
            next.moveTop(0);
        }
        if (edges_ & Qt::BottomEdge)
            next.setBottom(qMax(pressRect_.top() + minSize, pressRect_.bottom() + delta.y()));

        setPos(nextPos);
        setRect(next);
        if (scene()) scene()->update();
        event->accept();
    }

    void mouseReleaseEvent(QGraphicsSceneMouseEvent* event) override {
        resizing_ = false;
        edges_ = Qt::Edges();
        QGraphicsRectItem::mouseReleaseEvent(event);
    }

private:
    Qt::Edges hitEdges(const QPointF& point) const {
        const QRectF r = rect();
        constexpr qreal margin = 12.0;
        Qt::Edges edges;
        if (qAbs(point.x() - r.left()) <= margin) edges |= Qt::LeftEdge;
        if (qAbs(point.x() - r.right()) <= margin) edges |= Qt::RightEdge;
        if (qAbs(point.y() - r.top()) <= margin) edges |= Qt::TopEdge;
        if (qAbs(point.y() - r.bottom()) <= margin) edges |= Qt::BottomEdge;
        return edges;
    }

    bool resizing_;
    Qt::Edges edges_;
    QPointF pressPos_;
    QRectF pressRect_;
    QPointF pressItemPos_;
};

// OCR 选区遮罩
class SelectionMaskItem : public QGraphicsItem {
public:
    SelectionMaskItem(const QSizeF& imageSize, const ResizableRectItem* selection,
                      QGraphicsItem* parent = nullptr)
        : QGraphicsItem(parent), imageSize_(imageSize), selection_(selection) {}

    QRectF boundingRect() const override { return QRectF(QPointF(0, 0), imageSize_); }

    void paint(QPainter* painter, const QStyleOptionGraphicsItem*, QWidget*) override {
        const QRectF imageRect = boundingRect();
        const QRectF selected = selection_->sceneBoundingRect();
        const QColor shade(0, 0, 0, 170);
        painter->setPen(Qt::NoPen);
        painter->setBrush(shade);
        painter->drawRect(QRectF(imageRect.left(), imageRect.top(), imageRect.width(),
                                 qMax(0.0, selected.top() - imageRect.top())));
        painter->drawRect(QRectF(imageRect.left(), selected.bottom(), imageRect.width(),
                                 qMax(0.0, imageRect.bottom() - selected.bottom())));
        painter->drawRect(QRectF(imageRect.left(), selected.top(),
                                 qMax(0.0, selected.left() - imageRect.left()), selected.height()));
        painter->drawRect(QRectF(selected.right(), selected.top(),
                                 qMax(0.0, imageRect.right() - selected.right()), selected.height()));
    }

private:
    QSizeF imageSize_;
    const ResizableRectItem* selection_;
};

// 拍照按钮（圆形快门）的定义在 src/ui/widgets/capture_button.h —— 二维码已改实时扫描、
// 不再用快门，本页的 OCR 流程是当前唯一使用者
using uiwidgets::CaptureButton;

// 卡片点击事件过滤器
class CardClickFilter : public QObject {
    Q_OBJECT
public:
    CardClickFilter(std::function<void()> handler, QObject* parent = nullptr)
        : QObject(parent), handler_(std::move(handler)) {}

protected:
    bool eventFilter(QObject* obj, QEvent* event) override {
        if (event->type() == QEvent::MouseButtonRelease) {
            auto* frame = qobject_cast<QFrame*>(obj);
            if (frame && frame->cursor() == Qt::PointingHandCursor) {
                if (handler_) handler_();
                return true;
            }
        }
        return QObject::eventFilter(obj, event);
    }

private:
    std::function<void()> handler_;
};

} // namespace

// ============================================================================
// SmartRecognitionWidget 实现
// ============================================================================

SmartRecognitionWidget::SmartRecognitionWidget(const std::string& workspace,
                                               bool rollCallAvailable, bool equipmentAvailable,
                                               QWidget* parent)
    : QWidget(parent), workspace_(workspace) {
    setObjectName(QStringLiteral("smartRecognitionPage"));
    setStyleSheet(QStringLiteral("#smartRecognitionPage { background:%1; }").arg(theme::BG));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(24);

    auto* header = new QVBoxLayout;
    header->setSpacing(8);

    header->addWidget(textLabel(QStringLiteral("智能识别"), theme::FS_PAGE, theme::TEXT, this, true));
    header->addWidget(textLabel(QStringLiteral("识别工具与现场业务入口：二维码 / OCR / 人员点名 / 设备盘点"),
                                theme::FS_HINT, theme::TEXT_MUTED, this));

    root->addLayout(header);
    root->addSpacing(20);

    auto* centerContainer = new QHBoxLayout;
    centerContainer->addStretch(1);

    auto* buttonGrid = new QVBoxLayout;
    buttonGrid->setSpacing(28);

    auto* row1 = new QHBoxLayout;
    row1->setSpacing(36);

#ifdef HAS_ZBAR
    auto* qrBtn = createLargeButton(
        QStringLiteral("二维码识别"),
        QStringLiteral("拍摄画面中的二维码并解析内容"),
        theme::ACCENT, true);
#else
    auto* qrBtn = createLargeButton(
        QStringLiteral("二维码识别"),
        QStringLiteral("本机构建未启用 ZBar，无法使用"),
        theme::ACCENT, false);
#endif

    const bool modelsReady = ocrModelsReady();
    auto* ocrBtn = createLargeButton(
        QStringLiteral("OCR 识别"),
        modelsReady ? QStringLiteral("拍摄设备铭牌照片提取文字")
                    : QStringLiteral("模型文件未部署"),
        theme::SKY, modelsReady);

    row1->addWidget(qrBtn);
    row1->addWidget(ocrBtn);
    buttonGrid->addLayout(row1);

    auto* row2 = new QHBoxLayout;
    row2->setSpacing(36);
    auto* rollCallBtn = createLargeButton(
        QStringLiteral("人员点名"),
        rollCallAvailable ? QStringLiteral("创建点名任务，拍照并完成人脸识别")
                          : QStringLiteral("点名服务初始化失败，请检查人脸识别模型"),
        theme::SUCCESS, rollCallAvailable);
    auto* equipmentBtn = createLargeButton(
        QStringLiteral("设备盘点"),
        equipmentAvailable ? QStringLiteral("盘点设备资产，拍照完成目标检测与登记")
                           : QStringLiteral("盘点服务初始化失败，请检查设备检测模型"),
        theme::WARNING, equipmentAvailable);
    row2->addWidget(rollCallBtn);
    row2->addWidget(equipmentBtn);
    buttonGrid->addLayout(row2);

    buttonGrid->addStretch(2);

    centerContainer->addLayout(buttonGrid, 0);
    centerContainer->addStretch(1);

    root->addLayout(centerContainer, 1);
}

SmartRecognitionWidget::~SmartRecognitionWidget() = default;

QWidget* SmartRecognitionWidget::createLargeButton(const QString& title, const QString& description,
                                                   const char* accent, bool enabled) {
    auto* btn = new QFrame(this);
    btn->setObjectName(QStringLiteral("smartRecognitionCard"));
    btn->setFixedSize(680, 200);
    btn->setCursor(enabled ? Qt::PointingHandCursor : Qt::ForbiddenCursor);
    btn->setStyleSheet(QString(
        "QFrame#smartRecognitionCard { background: %1; border: 2px solid %2; border-radius: 12px; }"
        "QFrame#smartRecognitionCard:hover { border-color: %3; }"
        "QFrame#smartRecognitionCard QLabel { background: transparent; border: none; border-radius: 0; }")
        .arg(theme::PANEL, theme::BORDER, enabled ? accent : theme::BORDER));

    auto* layout = new QVBoxLayout(btn);
    layout->setContentsMargins(36, 30, 36, 30);
    layout->setSpacing(18);

    auto* titleRow = new QHBoxLayout;
    titleRow->setSpacing(16);
    auto* marker = new QLabel(QStringLiteral("●"), btn);
    marker->setStyleSheet(theme::text(enabled ? accent : theme::TEXT_MUTED, theme::FS_PAGE));
    titleRow->addWidget(marker, 0, Qt::AlignVCenter);
    titleRow->addWidget(textLabel(title, theme::FS_PAGE,
                                  enabled ? theme::TEXT : theme::TEXT_MUTED, btn, true),
                        1, Qt::AlignVCenter);
    layout->addLayout(titleRow);

    layout->addWidget(textLabel(description, theme::FS_BODY, theme::TEXT_MUTED, btn));

    layout->addStretch();

    // 点击分发按标题匹配；卡片处于置灰态时 cursor 不是 PointingHandCursor，
    // CardClickFilter 会自动忽略点击，所以置灰只需改 cursor 即可屏蔽交互。
    auto* filter = new CardClickFilter([this, title]() {
        if (title.contains(QStringLiteral("二维码"))) {
            openQrScanner();
        } else if (title.contains(QStringLiteral("OCR"))) {
            openOcrScanner();
        } else if (title.contains(QStringLiteral("点名"))) {
            emit rollCallRequested();
        } else if (title.contains(QStringLiteral("盘点"))) {
            emit equipmentRequested();
        }
    }, btn);
    btn->installEventFilter(filter);

    return btn;
}

QString SmartRecognitionWidget::ocrDir() const {
    return QString::fromStdString(workspace_) + QStringLiteral("/model/library/ppocrv4-ocr");
}

QString SmartRecognitionWidget::ocrBinDir() const {
    return QString::fromStdString(workspace_) + QStringLiteral("/3rdparty/ppocrv4/bin");
}

bool SmartRecognitionWidget::ocrModelsReady() const {
    const QString dir = ocrDir();
    const QString bindir = ocrBinDir();
    return QFileInfo(dir + QStringLiteral("/det.rknn")).isFile() &&
           QFileInfo(dir + QStringLiteral("/rec.rknn")).isFile() &&
           QFileInfo(dir + QStringLiteral("/labels.txt")).isFile() &&
           QFileInfo(bindir + QStringLiteral("/rknn_ppocr_det_demo")).isFile() &&
           QFileInfo(bindir + QStringLiteral("/rknn_ppocr_system_demo")).isFile();
}

// ============================================================================
// QR 扫描
// ============================================================================

void SmartRecognitionWidget::openQrScanner() {
    // 实时扫描 → 解出即冻结画面显示内容 → 用户点「确认」关窗格，整条流程在共用模块里
    // （src/ui/widgets/qr_scan_dialog.cpp）。登录页调的是同一个模块的 scan()，所以任何时刻
    // 只有一个 /dev/videoNN 持有者，两处也不会解出不一样的结果。
    QrScanDialog::openFullScanner(this);
}

// ============================================================================
// OCR 文字识别
// ============================================================================

void SmartRecognitionWidget::openOcrScanner() {
    ocrTrace(QStringLiteral("open scanner; checking models"));
    if (!ocrModelsReady()) {
        ocrTrace(QStringLiteral("models missing"));
        QMessageBox::information(this, QStringLiteral("OCR 暂不可用"),
                                 QStringLiteral("缺少 OCR 文件，无法识别。\n"
                                                "模型：model/library/ppocrv4-ocr/ 下的 det.rknn、rec.rknn\n"
                                                "可执行：3rdparty/ppocrv4/bin/ 下的 "
                                                "rknn_ppocr_det_demo、rknn_ppocr_system_demo\n"
                                                "构建与部署步骤见 3rdparty/ppocrv4/README.md。"));
        return;
    }
    ocrTrace(QStringLiteral("models ready; opening photo dialog"));
    showOcrPhotoDialog();
}

void SmartRecognitionWidget::showOcrPhotoDialog() {
    ocrTrace(QStringLiteral("photo dialog setup begin"));
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("OCR 识别 - 拍照"));
    dialog.resize(960, 720);
    dialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(0, 0, 0, 0);

    auto* previewWidget = new GLVideoWidget(&dialog);
    root->addWidget(previewWidget, 1);

    auto* captureBtn = new CaptureButton(previewWidget);
    auto positionCapture = [captureBtn, previewWidget]() {
        captureBtn->move((previewWidget->width() - captureBtn->width()) / 2,
                         previewWidget->height() - captureBtn->height() - 24);
        captureBtn->raise();
    };
    positionCapture();
    QTimer::singleShot(0, positionCapture);
    captureBtn->setToolTip(QStringLiteral("拍照"));

    ocrTrace(QStringLiteral("creating camera decoder"));
    const QString cameraUrl = ConfigManager::instance().smartRecognitionCamera();
    // 只取画面帧给 ZBar / OCR，不参与级联推理：PreviewOnly 不加载任何 RKNN 模型
    auto* decoder = new FFmpegVideoDecoder(
        nullptr, FFmpegVideoDecoder::InferenceMode::PreviewOnly);
    QObject::connect(decoder, &FFmpegVideoDecoder::frameReady,
                     previewWidget, &GLVideoWidget::onFrameReady, Qt::DirectConnection);
    ocrTrace(QStringLiteral("camera start begin: %1").arg(cameraUrl));
    decoder->start(cameraUrl);
    ocrTrace(QStringLiteral("camera start returned"));

    QString capturedImagePath;

    connect(captureBtn, &QPushButton::clicked, [this, decoder, &capturedImagePath, &dialog]() {
        ocrTrace(QStringLiteral("capture clicked; captureFrameAsImage begin"));
        QImage frameImage = captureFrameAsImage(decoder);
        ocrTrace(QStringLiteral("captureFrameAsImage returned: %1x%2 null=%3")
                     .arg(frameImage.width()).arg(frameImage.height()).arg(frameImage.isNull()));
        if (frameImage.isNull()) {
            QMessageBox::warning(nullptr, QStringLiteral("错误"), QStringLiteral("无法获取相机画面"));
            return;
        }

        QTemporaryFile tempFile(QStringLiteral("/tmp/ocr_capture_XXXXXX.jpg"));
        tempFile.setAutoRemove(false);
        if (!tempFile.open()) {
            QMessageBox::warning(nullptr, QStringLiteral("错误"), QStringLiteral("无法创建临时文件"));
            return;
        }
        if (capturedImagePath.startsWith(QStringLiteral("/tmp/ocr_"))) QFile::remove(capturedImagePath);
        capturedImagePath = tempFile.fileName();
        tempFile.close();

        ocrTrace(QStringLiteral("capture temp file ready: %1; saving begin").arg(capturedImagePath));
        if (!frameImage.save(capturedImagePath, "JPG", 95)) {
            ocrTrace(QStringLiteral("capture JPG save failed"));
            QFile::remove(capturedImagePath);
            capturedImagePath.clear();
            QMessageBox::warning(nullptr, QStringLiteral("错误"), QStringLiteral("无法保存照片"));
            return;
        }

        ocrTrace(QStringLiteral("capture JPG saved; confirmation dialog begin"));
        QDialog confirm(&dialog);
        confirm.setWindowTitle(QStringLiteral("确认照片"));
        confirm.resize(860, 620);
        auto* confirmLayout = new QVBoxLayout(&confirm);
        auto* imageLabel = new QLabel(&confirm);
        imageLabel->setAlignment(Qt::AlignCenter);
        imageLabel->setPixmap(QPixmap(capturedImagePath).scaled(820, 520, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        confirmLayout->addWidget(imageLabel, 1);
        auto* confirmButtons = new QHBoxLayout;
        auto* cancel = new QPushButton(QStringLiteral("取消"), &confirm);
        auto* accept = new QPushButton(QStringLiteral("确认"), &confirm);
        cancel->setMinimumHeight(46);
        accept->setMinimumHeight(46);
        confirmButtons->addWidget(cancel);
        confirmButtons->addStretch();
        confirmButtons->addWidget(accept);
        confirmLayout->addLayout(confirmButtons);
        connect(cancel, &QPushButton::clicked, &confirm, &QDialog::reject);
        connect(accept, &QPushButton::clicked, &confirm, &QDialog::accept);
        if (confirm.exec() != QDialog::Accepted) {
            ocrTrace(QStringLiteral("capture confirmation cancelled; returning to camera"));
            QFile::remove(capturedImagePath);
            capturedImagePath.clear();
            return;
        }
        ocrTrace(QStringLiteral("capture confirmed; camera stop begin"));
        decoder->stop();
        dialog.accept();
        showOcrRegionDialog(capturedImagePath);
    });

    ocrTrace(QStringLiteral("photo dialog exec begin"));
    dialog.exec();
    ocrTrace(QStringLiteral("photo dialog exec returned; camera cleanup begin"));

    decoder->stop();
    delete decoder;
    if (capturedImagePath.startsWith(QStringLiteral("/tmp/ocr_"))) QFile::remove(capturedImagePath);
    ocrTrace(QStringLiteral("camera cleanup complete"));
}

void SmartRecognitionWidget::showOcrRegionDialog(const QString& imagePath) {
    ocrTrace(QStringLiteral("region JPG QPixmap load begin: %1").arg(imagePath));
    const QPixmap pixmap(imagePath);
    ocrTrace(QStringLiteral("region JPG QPixmap load returned: null=%1 size=%2x%3")
                 .arg(pixmap.isNull()).arg(pixmap.width()).arg(pixmap.height()));
    if (pixmap.isNull()) {
        QMessageBox::warning(this, QStringLiteral("识别失败"), QStringLiteral("无法加载照片"));
        return;
    }
    ocrTrace(QStringLiteral("pixmap.toImage begin"));
    const QImage image = pixmap.toImage();
    ocrTrace(QStringLiteral("pixmap.toImage returned; detector begin"));
    auto boxes = runPpocrDet(imagePath);
    ocrTrace(QStringLiteral("detector returned: boxes=%1").arg(boxes.size()));
    if (boxes.empty()) {
        QMessageBox::warning(this, QStringLiteral("识别失败"), QStringLiteral("未检测到文本区域，请重新拍照"));
        return;
    }

    ocrTrace(QStringLiteral("grouping boxes begin"));
    auto regions = smartGroupBoxes(boxes, image.width(), image.height());
    cv::Rect defaultRegion = selectDefaultRegion(regions, image.width(), image.height());
    ocrTrace(QStringLiteral("grouping complete: regions=%1").arg(regions.size()));

    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("OCR 识别 - 选择区域"));
    dialog.resize(960, 700);
    dialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 20, 20, 20);

    root->addWidget(textLabel(QStringLiteral("调整识别区域"), theme::FS_PAGE, theme::TEXT, &dialog, true));

    root->addWidget(textLabel(
        QStringLiteral("绿色框为推荐区域，可拖动调整。灰色框为检测到的所有文本区域（参考）。"),
        theme::FS_HINT, theme::TEXT_MUTED, &dialog));

    auto* scene = new QGraphicsScene(&dialog);
    auto* view = new QGraphicsView(scene, &dialog);
    view->setMinimumHeight(500);
    view->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    view->setStyleSheet(QString("background:%1; border:1px solid %2;")
                            .arg(theme::FIELD, theme::BORDER));

    scene->addPixmap(pixmap);
    scene->setSceneRect(0, 0, image.width(), image.height());
    auto* candidateRect = new ResizableRectItem(
        QRectF(defaultRegion.x, defaultRegion.y, defaultRegion.width, defaultRegion.height));
    scene->addItem(candidateRect);
    auto* mask = new SelectionMaskItem(QSizeF(image.size()), candidateRect);
    scene->addItem(mask);
    mask->setZValue(1);
    candidateRect->setPen(QPen(Qt::white, 8));
    candidateRect->setZValue(2);
    QTimer::singleShot(0, &dialog, [view, scene]() {
        view->fitInView(scene->sceneRect(), Qt::KeepAspectRatio);
    });

    root->addWidget(view, 1);

    auto* buttons = new QHBoxLayout;
    auto* backBtn = new QPushButton(QStringLiteral("重新拍照"), &dialog);
    auto* confirmBtn = new QPushButton(QStringLiteral("确认识别"), &dialog);

    backBtn->setMinimumHeight(46);
    confirmBtn->setMinimumHeight(46);
    backBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:%2; border:none; border-radius:12px; padding:8px 24px; font-size:%3px; font-weight:600; }"
        "QPushButton:hover { background:%4; } QPushButton:pressed { background:%5; }")
        .arg(theme::HOVER, theme::TEXT)
        .arg(theme::FS_CARD)
        .arg(theme::BORDER, theme::PANEL));
    confirmBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:%2; border:none; border-radius:12px; padding:8px 32px; font-size:%3px; font-weight:700; }"
        "QPushButton:hover { background:%4; } QPushButton:pressed { background:%5; }")
        .arg(theme::ACCENT, theme::BG)
        .arg(theme::FS_CARD)
        .arg(theme::SKY, theme::SKY_PRESSED));

    buttons->addWidget(backBtn);
    buttons->addStretch();
    buttons->addWidget(confirmBtn);
    root->addLayout(buttons);

    connect(backBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    connect(confirmBtn, &QPushButton::clicked, [this, &dialog, image, scene, candidateRect]() {
        ocrTrace(QStringLiteral("confirm clicked; calculating crop"));
        const QRectF selectedSceneRect = candidateRect->sceneBoundingRect()
                                             .intersected(scene->sceneRect());
        QRect cropRect(qRound(selectedSceneRect.left()), qRound(selectedSceneRect.top()),
                       qRound(selectedSceneRect.width()), qRound(selectedSceneRect.height()));
        cropRect = cropRect.intersected(image.rect());
        if (cropRect.isEmpty()) {
            QMessageBox::warning(&dialog, QStringLiteral("错误"), QStringLiteral("请选择有效区域"));
            return;
        }
        ocrTrace(QStringLiteral("crop image begin"));
        QImage cropped = image.copy(cropRect).convertToFormat(QImage::Format_RGB888);

        QTemporaryFile tempFile(QStringLiteral("/tmp/ocr_roi_XXXXXX.jpg"));
        tempFile.setAutoRemove(false);
        if (!tempFile.open()) {
            QMessageBox::warning(&dialog, QStringLiteral("错误"), QStringLiteral("无法创建临时文件"));
            return;
        }
        QString croppedPath = tempFile.fileName();
        tempFile.close();
        if (!cropped.save(croppedPath, "JPG", 95)) {
            QFile::remove(croppedPath);
            QMessageBox::warning(&dialog, QStringLiteral("错误"), QStringLiteral("无法保存识别区域"));
            return;
        }
        ocrTrace(QStringLiteral("crop saved; recognizer begin"));

        dialog.accept();

        auto texts = runPpocrSystem(croppedPath);
        ocrTrace(QStringLiteral("recognizer returned: texts=%1").arg(texts.size()));
        if (texts.isEmpty()) {
            QMessageBox::warning(this, QStringLiteral("识别失败"), QStringLiteral("未识别到文字，请调整选区后重试"));
        } else {
            showOcrResultDialog(texts);
        }

        QFile::remove(croppedPath);
        ocrTrace(QStringLiteral("crop cleanup complete"));
    });

    ocrTrace(QStringLiteral("region dialog exec begin"));
    dialog.exec();
    ocrTrace(QStringLiteral("region dialog exec returned"));
}

void SmartRecognitionWidget::showOcrResultDialog(const QStringList& texts) {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("OCR 识别结果"));
    dialog.resize(600, 500);
    dialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(20, 20, 20, 20);

    root->addWidget(textLabel(QStringLiteral("识别结果"), theme::FS_PAGE, theme::TEXT, &dialog, true));

    auto* scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    scrollArea->setStyleSheet(QString("QScrollArea { background:%1; border:1px solid %2; border-radius:8px; }")
                                  .arg(theme::FIELD, theme::BORDER));

    auto* contentWidget = new QWidget;
    auto* contentLayout = new QVBoxLayout(contentWidget);
    contentLayout->setContentsMargins(16, 16, 16, 16);
    auto* resultLabel = textLabel(texts.join(QStringLiteral("\n")), theme::FS_CARD, theme::TEXT, contentWidget);
    resultLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addWidget(resultLabel);
    contentLayout->addStretch();
    contentWidget->setStyleSheet(QString("QWidget { background:%1; border:1px solid %2; border-radius:8px; }")
                                     .arg(theme::PANEL, theme::BORDER));
    scrollArea->setWidget(contentWidget);
    root->addWidget(scrollArea, 1);
    // 这个对话框是按需 new 的，frmMain 构造末尾那次全量扫描扫不到它，得单独挂。
    // 代价：上面那句 TextSelectableByMouse 的"按住拖选文字"在这里失效
    // （QScroller 接管 viewport 的左键拖动）。十寸屏上本来就没有 Ctrl+C 的入口
    // （本轮明确不做虚拟键盘），拖选留着的实际收益是零，而结果文字超过一屏时
    // 划动是真需求 —— 与日志区那一档取舍同一条线，见 src/ui/core/README.md。
    QtHelper::enableTouchScroll(scrollArea);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setMinimumHeight(46);
    buttons->button(QDialogButtonBox::Ok)->setStyleSheet(QString(
        "QPushButton { background:%1; color:%2; border:none; border-radius:12px; padding:0 24px; font-size:%3px; font-weight:700; }"
        "QPushButton:hover { background:%4; } QPushButton:pressed { background:%5; }")
        .arg(theme::ACCENT, theme::BG)
        .arg(theme::FS_CARD)
        .arg(theme::SKY, theme::SKY_PRESSED));
    root->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);

    dialog.exec();
}

// ============================================================================
// PaddleOCR 外部进程调用
// ============================================================================

std::vector<cv::Rect> SmartRecognitionWidget::runPpocrDet(const QString& imagePath) {
    std::vector<cv::Rect> boxes;

    const QString dir = ocrDir();
    QString program = ocrBinDir() + QStringLiteral("/rknn_ppocr_det_demo");
    QString modelPath = dir + QStringLiteral("/det.rknn");

    QProcess process;
    process.setWorkingDirectory(QDir::tempPath());  // demo 会在 CWD 落一张 out.jpg
    ocrTrace(QStringLiteral("det process start: program=%1 model=%2 image=%3")
                 .arg(program, modelPath, imagePath));
    process.start(program, {modelPath, imagePath});
    if (!process.waitForStarted(2000)) {
        ocrTrace(QStringLiteral("det process start failed: %1").arg(process.errorString()));
        qWarning() << "Unable to start ppocr-det:" << process.errorString();
        return boxes;
    }

    const bool finished = process.waitForFinished(10000);
    ocrTrace(QStringLiteral("det wait returned: finished=%1 status=%2 code=%3")
                 .arg(finished).arg(process.exitStatus()).arg(process.exitCode()));
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "ppocr-det timeout or failed";
        return boxes;
    }

    QString output = QString::fromUtf8(process.readAllStandardOutput());
    QStringList lines = output.split('\n');

    QRegExp rx("\\[\\d+\\]:\\s*\\[\\((\\d+),\\s*(\\d+)\\),\\s*\\((\\d+),\\s*(\\d+)\\),\\s*\\((\\d+),\\s*(\\d+)\\),\\s*\\((\\d+),\\s*(\\d+)\\)\\]");

    for (const QString& line : lines) {
        if (rx.indexIn(line) != -1) {
            int x1 = rx.cap(1).toInt(), y1 = rx.cap(2).toInt();
            int x2 = rx.cap(3).toInt(), y2 = rx.cap(4).toInt();
            int x3 = rx.cap(5).toInt(), y3 = rx.cap(6).toInt();
            int x4 = rx.cap(7).toInt(), y4 = rx.cap(8).toInt();

            int minX = std::min({x1, x2, x3, x4});
            int maxX = std::max({x1, x2, x3, x4});
            int minY = std::min({y1, y2, y3, y4});
            int maxY = std::max({y1, y2, y3, y4});

            boxes.emplace_back(minX, minY, maxX - minX, maxY - minY);
        }
    }

    ocrTrace(QStringLiteral("det parsing complete: boxes=%1").arg(boxes.size()));
    return boxes;
}

std::vector<cv::Rect> SmartRecognitionWidget::smartGroupBoxes(const std::vector<cv::Rect>& boxes,
                                                               int imageWidth, int imageHeight) {
    if (boxes.empty()) return {};

    std::vector<cv::Rect> sorted = boxes;
    std::sort(sorted.begin(), sorted.end(), [](const cv::Rect& a, const cv::Rect& b) {
        return a.y < b.y;
    });

    // 同一行的判据：与当前组最后一个框的顶边 y 差不足 30px。
    // 阈值按 det 输出的是"字/词"框、铭牌行高通常 >40px 取；换分辨率要重调。
    const int Y_THRESHOLD = 30;
    std::vector<std::vector<cv::Rect>> groups;
    std::vector<cv::Rect> currentGroup = {sorted[0]};

    for (size_t i = 1; i < sorted.size(); ++i) {
        if (std::abs(sorted[i].y - currentGroup.back().y) < Y_THRESHOLD) {
            currentGroup.push_back(sorted[i]);
        } else {
            groups.push_back(currentGroup);
            currentGroup = {sorted[i]};
        }
    }
    groups.push_back(currentGroup);

    std::vector<cv::Rect> regions;
    for (const auto& group : groups) {
        int minX = imageWidth, minY = imageHeight, maxX = 0, maxY = 0;
        for (const auto& box : group) {
            minX = std::min(minX, box.x);
            minY = std::min(minY, box.y);
            maxX = std::max(maxX, box.x + box.width);
            maxY = std::max(maxY, box.y + box.height);
        }
        regions.emplace_back(minX, minY, maxX - minX, maxY - minY);
    }

    return regions;
}

cv::Rect SmartRecognitionWidget::selectDefaultRegion(const std::vector<cv::Rect>& regions,
                                                     int imageWidth, int imageHeight) {
    if (regions.empty()) {
        return cv::Rect(0, 0, imageWidth, imageHeight);
    }

    cv::Point2f center(imageWidth / 2.0f, imageHeight / 2.0f);
    cv::Rect closest = regions[0];
    float minDist = std::numeric_limits<float>::max();

    for (const auto& region : regions) {
        cv::Point2f regionCenter(region.x + region.width / 2.0f,
                                region.y + region.height / 2.0f);
        float dist = cv::norm(regionCenter - center);
        if (dist < minDist) {
            minDist = dist;
            closest = region;
        }
    }

    return closest;
}

QStringList SmartRecognitionWidget::runPpocrSystem(const QString& imagePath) {
    QStringList results;

    const QString dir = ocrDir();
    QString program = ocrBinDir() + QStringLiteral("/rknn_ppocr_system_demo");
    QString detModel = dir + QStringLiteral("/det.rknn");
    QString recModel = dir + QStringLiteral("/rec.rknn");

    QProcess process;
    process.setWorkingDirectory(QDir::tempPath());  // demo 会在 CWD 落一张 out.jpg
    ocrTrace(QStringLiteral("system process start: program=%1 det=%2 rec=%3 image=%4")
                 .arg(program, detModel, recModel, imagePath));
    process.start(program, {detModel, recModel, imagePath});
    if (!process.waitForStarted(2000)) {
        ocrTrace(QStringLiteral("system process start failed: %1").arg(process.errorString()));
        qWarning() << "Unable to start ppocr-system:" << process.errorString();
        return results;
    }

    const bool finished = process.waitForFinished(15000);
    ocrTrace(QStringLiteral("system wait returned: finished=%1 status=%2 code=%3")
                 .arg(finished).arg(process.exitStatus()).arg(process.exitCode()));
    if (!finished || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        qWarning() << "ppocr-system timeout or failed";
        return results;
    }

    QString output = QString::fromUtf8(process.readAllStandardOutput());
    QStringList lines = output.split('\n');

    for (const QString& line : lines) {
        int start = line.indexOf(QStringLiteral("regconize result: "));
        int end = line.indexOf(QStringLiteral(", score="));

        if (start != -1 && end != -1 && end > start) {
            start += 18;
            QString text = line.mid(start, end - start).trimmed();
            if (!text.isEmpty()) {
                results.append(text);
            }
        }
    }

    ocrTrace(QStringLiteral("system parsing complete: texts=%1").arg(results.size()));
    return results;
}

// moc 包含（匿名命名空间中的 Q_OBJECT 类需要）
#include "smart_recognition_widget.moc"
