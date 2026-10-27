#pragma execution_character_set("utf-8")

/**
 * @file smart_recognition_widget.cpp
 * @brief 智能识别页实现 —— 二维码识别（ZBar）+ 铭牌文字识别（PP-OCRv4）
 *
 * 页面本身只是一张两卡片的入口页（setupUi），真正的交互都在两个模态对话框里：
 *   1. 二维码：openQrScanner() 开一台独立 FFmpegVideoDecoder 做预览，另起
 *      QRScanWorker 线程每 100ms 拉一帧给 ZBar 解码；QRStabilityChecker 要求
 *      连续 3 次扫描出的**内容集合**（排序后）完全一致才认作稳定，避免把
 *      半帧、运动模糊的误读弹出来。单码直接画圈 + 弹结果框，多码进冻结图
 *      选区模式（点箭头按钮切换看第几个）。
 *   2. OCR：openOcrScanner() → showOcrPhotoDialog()（拍照存临时 jpg）→
 *      showOcrRegionDialog()（跑 det 拿文本框、smartGroupBoxes() 合并成可选区、
 *      用户可拖拽调框）→ showOcrResultDialog()（跑 system 输出 CTC 解码文本）。
 *      两阶段都不在本进程内推理，而是 QProcess 起 3rdparty/ppocrv4/bin 的
 *      rknn_model_zoo 官方 demo 并解析 stdout，详见 3rdparty/ppocrv4/README.md。
 *
 * 三条容易踩的前提：
 *   - 二维码能力取决于编译期有没有 HAS_ZBAR（cmake 找到 3rdparty/zbar 才定义），
 *     没有则该卡片直接禁用，页面其余部分照常。
 *   - 两个对话框各开一台解码器，取流地址都读 config.json 的
 *     smartRecognition.camera，与监控页/国标推流的设备之间没有互斥。
 *   - QTimer::singleShot 一律带 &dialog 作上下文对象：回调按引用捕获了 dialog
 *     的栈上成员，缺上下文时"识别到弹窗之间的 500ms 内关掉对话框"会 SIGSEGV。
 *
 * 本文件内有多个 Q_OBJECT 类定义在匿名 namespace，故末尾必须
 * #include "smart_recognition_widget.moc"，漏掉会连不过。
 */

#include "smart_recognition_widget.h"
#include "ffmpeg_video_decoder.h"
#include "gl_video_widget.h"
#include "theme.h"
#include "ConfigManager.h"

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
#include <QThread>
#include <QVBoxLayout>
#include <QRegExp>
#include <QPropertyAnimation>
#include <QPainter>
#include <QMouseEvent>
#include <QImage>

#include <opencv2/opencv.hpp>

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>

#ifdef HAS_ZBAR
#include <zbar.h>
#endif

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
// 帧捕获辅助：从 FFmpegVideoDecoder 的映射指针读取 RGBA 帧数据
// ============================================================================

// 从解码器映射指针获取 RGBA 帧，转为 QImage（RGB888）
QImage captureFrameAsImage(FFmpegVideoDecoder* decoder) {
    const uint8_t* ptr = decoder->captureCurrentFramePtr();
    if (!ptr) return {};
    int w, h, stride;
    if (!decoder->getCurrentFrameInfo(w, h, stride)) return {};

    QImage img(w, h, QImage::Format_RGB888);
    for (int y = 0; y < h; ++y) {
        const uint8_t* srcRow = ptr + y * stride;
        uint8_t* dstRow = img.scanLine(y);
        for (int x = 0; x < w; ++x) {
            dstRow[x * 3 + 0] = srcRow[x * 4 + 0];
            dstRow[x * 3 + 1] = srcRow[x * 4 + 1];
            dstRow[x * 3 + 2] = srcRow[x * 4 + 2];
        }
    }
    return img;
}

#ifdef HAS_ZBAR
// ============================================================================
// QR 码扫描相关（需要 ZBar 库）
// ============================================================================

struct QRCodeDetection {
    cv::Rect rect;
    QString content;
};

// 从 RGBA 原始数据直接转灰度并扫描（避免 OpenCV Mat 的 stride 问题）
std::vector<QRCodeDetection> scanQRCodeFromRGBA(const uint8_t* rgba, int width, int height, int stride) {
    std::vector<QRCodeDetection> results;
    if (!rgba || width <= 0 || height <= 0) return results;

    // RGBA → 灰度（ITU-R BT.601 系数）
    std::vector<uint8_t> gray(width * height);
    for (int y = 0; y < height; ++y) {
        const uint8_t* srcRow = rgba + y * stride;
        uint8_t* dstRow = gray.data() + y * width;
        for (int x = 0; x < width; ++x) {
            int r = srcRow[x * 4];
            int g = srcRow[x * 4 + 1];
            int b = srcRow[x * 4 + 2];
            dstRow[x] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        }
    }

    zbar::ImageScanner scanner;
    scanner.set_config(zbar::ZBAR_QRCODE, zbar::ZBAR_CFG_ENABLE, 1);

    zbar::Image zbar_image(width, height, "Y800", gray.data(), width * height);
    scanner.scan(zbar_image);

    for (zbar::Image::SymbolIterator symbol = zbar_image.symbol_begin();
         symbol != zbar_image.symbol_end(); ++symbol) {
        QRCodeDetection detection;

        std::string rawData = symbol->get_data();
        detection.content = QString::fromUtf8(rawData.c_str(), rawData.size());

        int minX = width, minY = height, maxX = 0, maxY = 0;
        for (int i = 0; i < symbol->get_location_size(); ++i) {
            minX = std::min(minX, symbol->get_location_x(i));
            maxX = std::max(maxX, symbol->get_location_x(i));
            minY = std::min(minY, symbol->get_location_y(i));
            maxY = std::max(maxY, symbol->get_location_y(i));
        }
        detection.rect = cv::Rect(minX, minY, maxX - minX, maxY - minY);
        results.push_back(detection);
    }

    return results;
}

// 稳定性检测：连续 3 次一致结果才确认
class QRStabilityChecker {
public:
    bool check(const std::vector<QRCodeDetection>& current) {
        if (current.empty()) {
            history_.clear();
            return false;
        }

        QStringList contents;
        for (const auto& qr : current) contents.append(qr.content);
        contents.sort();

        history_.push_back(contents);
        if (history_.size() > 3) history_.pop_front();

        if (history_.size() == 3) {
            return history_[0] == history_[1] && history_[1] == history_[2];
        }
        return false;
    }

    void reset() { history_.clear(); }

private:
    std::deque<QStringList> history_;
};

// QR 扫描工作线程：每 100ms 从解码器取帧扫描
class QRScanWorker : public QObject {
    Q_OBJECT
public:
    explicit QRScanWorker(FFmpegVideoDecoder* decoder) : decoder_(decoder) {}

public slots:
    void start() {
        if (timer_) {
            timer_->start();
            return;
        }
        timer_ = new QTimer(this);
        timer_->setInterval(100);
        connect(timer_, &QTimer::timeout, this, &QRScanWorker::scanOnce);
        timer_->start();
    }

    void stop() {
        if (timer_) timer_->stop();
    }

private slots:
    void scanOnce() {
        const uint8_t* ptr = decoder_->captureCurrentFramePtr();
        if (!ptr) {
            checker_.reset();
            return;
        }
        int w, h, stride;
        if (!decoder_->getCurrentFrameInfo(w, h, stride)) {
            checker_.reset();
            return;
        }

        const auto detections = scanQRCodeFromRGBA(ptr, w, h, stride);
        if (checker_.check(detections)) {
            QImage frameImage = captureFrameAsImage(decoder_);
            emit stableDetections(detections, w, h, frameImage);
        }
    }

signals:
    void stableDetections(std::vector<QRCodeDetection> detections, int frameWidth,
                          int frameHeight, QImage frameImage);

private:
    FFmpegVideoDecoder* decoder_;
    QTimer* timer_ = nullptr;
    QRStabilityChecker checker_;
};
#endif // HAS_ZBAR

// ============================================================================
// UI 辅助控件
// ============================================================================

// 扫描线动画
class ScanLineWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int yPosition READ yPosition WRITE setYPosition)
public:
    explicit ScanLineWidget(QWidget* parent = nullptr)
        : QWidget(parent), yPosition_(0) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        animation_ = new QPropertyAnimation(this, "yPosition", this);
        animation_->setDuration(2500);
        animation_->setLoopCount(-1);
        animation_->setEasingCurve(QEasingCurve::Linear);
    }

    void start(int height) {
        animation_->setStartValue(0);
        animation_->setEndValue(height);
        animation_->start();
    }

    void stop() { animation_->stop(); }

    int yPosition() const { return yPosition_; }
    void setYPosition(int pos) {
        yPosition_ = pos;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        const int lineThickness = 3;
        const int trailLength = 80;

        // 扫描线取全站强调色，透明度做拖尾
        const QColor accent(theme::ACCENT);
        const auto fade = [accent](int alpha) {
            QColor c = accent;
            c.setAlpha(alpha);
            return c;
        };
        QLinearGradient gradient(0, yPosition_ - trailLength, 0, yPosition_ + lineThickness);
        gradient.setColorAt(0, fade(0));
        gradient.setColorAt(0.3, fade(100));
        gradient.setColorAt(0.7, fade(255));
        gradient.setColorAt(1, fade(180));

        painter.fillRect(0, yPosition_ - trailLength, width(), trailLength + lineThickness, gradient);
    }

private:
    QPropertyAnimation* animation_;
    int yPosition_;
};

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

// 拍照按钮（圆形快门）
class CaptureButton : public QPushButton {
public:
    explicit CaptureButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setFixedSize(88, 88);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF outer = rect().adjusted(2, 2, -2, -2);
        painter.setPen(QPen(QColor(255, 255, 255, 190), 5));
        painter.setBrush(Qt::white);
        painter.drawEllipse(outer);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(theme::BG));
        painter.drawEllipse(outer.adjusted(20, 20, -20, -20));
    }
};

#ifdef HAS_ZBAR
// 单码标记：绿色圆点
class CircleMarker : public QWidget {
public:
    explicit CircleMarker(QWidget* parent = nullptr) : QWidget(parent) {
        setFixedSize(64, 64);
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 4));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(rect().adjusted(4, 4, -4, -4));
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(theme::SUCCESS));
        painter.drawEllipse(rect().adjusted(16, 16, -16, -16));
    }
};

// 多码标记：箭头按钮
class ArrowButton : public QPushButton {
public:
    explicit ArrowButton(QWidget* parent = nullptr) : QPushButton(parent) {
        setFixedSize(72, 72);
        setCursor(Qt::PointingHandCursor);
        setStyleSheet(QString(
            "QPushButton { background: %1; border: 5px solid white; border-radius: 36px; }"
            "QPushButton:hover { background: %2; }")
            .arg(theme::SUCCESS, theme::SUCCESS_HOVER));
    }

protected:
    void paintEvent(QPaintEvent*) override {
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
#endif // HAS_ZBAR

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

SmartRecognitionWidget::SmartRecognitionWidget(const std::string& workspace, QWidget* parent)
    : QWidget(parent), workspace_(workspace) {
    setObjectName(QStringLiteral("smartRecognitionPage"));
    setStyleSheet(QStringLiteral("#smartRecognitionPage { background:%1; }").arg(theme::BG));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(40, 40, 40, 40);
    root->setSpacing(24);

    auto* header = new QVBoxLayout;
    header->setSpacing(8);

    header->addWidget(textLabel(QStringLiteral("智能识别"), theme::FS_PAGE, theme::TEXT, this, true));
    header->addWidget(textLabel(QStringLiteral("选择识别方式，快速读取二维码或设备铭牌信息"),
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
        QStringLiteral("实时扫描画面中的二维码"),
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

    if (enabled) {
        auto clickHandler = [this, title]() {
            if (title.contains(QStringLiteral("二维码"))) {
                openQrScanner();
            } else {
                openOcrScanner();
            }
        };
        auto* filter = new CardClickFilter(clickHandler, btn);
        btn->installEventFilter(filter);
    }

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
#ifndef HAS_ZBAR
    // 不是"板子上没装 zbar"，而是这次 cmake 没找到 3rdparty/zbar：
    // 新克隆只带头文件和 zbar.pc，libzbar.so/.a 被 .gitignore 挡住，需照
    // 3rdparty/README.md 的流程补进 3rdparty/zbar/lib/ 后重新 configure。
    QMessageBox::information(this, QStringLiteral("不可用"),
                             QStringLiteral("本次构建未找到 ZBar（3rdparty/zbar/lib 缺库），"
                                            "补齐后重新编译才能扫码。"));
    return;
#else
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("扫描二维码"));
    dialog.resize(960, 640);
    dialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto* root = new QVBoxLayout(&dialog);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto* previewContainer = new QWidget(&dialog);
    // 视频留边用纯黑，非主题色板成员
    previewContainer->setStyleSheet(QStringLiteral("background:#000000;"));
    auto* previewLayout = new QVBoxLayout(previewContainer);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    auto* previewWidget = new GLVideoWidget(previewContainer);
    previewWidget->setMinimumSize(960, 540);
    previewLayout->addWidget(previewWidget);

    auto* scanLine = new ScanLineWidget(previewWidget);
    scanLine->setGeometry(0, 0, previewWidget->width(), previewWidget->height());
    scanLine->raise();

    root->addWidget(previewContainer, 1);

    auto* hintLabel = new QLabel(QStringLiteral("扫描到多个二维码，请选择一个打开"), &dialog);
    hintLabel->setAlignment(Qt::AlignCenter);
    hintLabel->setStyleSheet(QString(
        "color:%1; font-size:%2px; font-weight:500; background:%3; padding:14px;"
        "border-top:2px solid %4;")
        .arg(theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::PANEL, theme::BORDER));
    hintLabel->hide();
    root->addWidget(hintLabel);

    auto* closeBtn = new QPushButton(QStringLiteral("关闭"), &dialog);
    closeBtn->setMinimumHeight(50);
    closeBtn->setStyleSheet(QString(
        "QPushButton { background:%1; color:%2; border:none; font-size:%3px; font-weight:600; }"
        "QPushButton:hover { background:%4; } QPushButton:pressed { background:%5; }")
        .arg(theme::HOVER, theme::TEXT)
        .arg(theme::FS_BODY)
        .arg(theme::BORDER, theme::PANEL));
    root->addWidget(closeBtn);

    connect(closeBtn, &QPushButton::clicked, &dialog, &QDialog::reject);

    // 本对话框自带一台独立解码器，与监控主链路的 4 路互不共享：
    // 同一台设备被两处同时开 v4l2 会失败，所以 smartRecognition.camera 指的
    // 设备在扫码期间不能再被其它页占用（见 src/base/config/README.md）。
    // decoder 无 parent，收尾处显式 stop + delete（见 exec() 返回之后）。
    const QString cameraUrl = ConfigManager::instance().smartRecognitionCamera();
    auto* decoder = new FFmpegVideoDecoder();
    // DirectConnection 与监控页一致（见 src/ui/form/README.md 的线程说明）：
    // frameReady 由解码线程发出，onFrameReady 只在解码线程做"加锁换帧 + update()"，
    // 真正的 GL 绘制仍在 GUI 线程的 paintGL 里。用 Queued 反而会把帧多留一个事件循环。
    QObject::connect(decoder, &FFmpegVideoDecoder::frameReady,
                     previewWidget, &GLVideoWidget::onFrameReady, Qt::DirectConnection);
    decoder->start(cameraUrl);

    // 上下文对象必须是 dialog：回调捕获的是 dialog 的栈上成员（scanLine 等），
    // 用户在 100ms 内关闭对话框时 dialog 已析构，无上下文的 singleShot 会解引用野指针。
    QTimer::singleShot(100, &dialog, [scanLine, previewWidget]() {
        scanLine->start(previewWidget->height());
    });

    // 扫码放独立线程：ZBar 解一帧是几十毫秒级 CPU 活，留在 GUI 线程会让画面
    // 直接卡住（本轮卡顿修复刚把 GUI 线程腾空，别再填回去）。
    // worker 复用同一台 decoder，通过 captureCurrentFramePtr() 跨线程拉最新帧。
    qRegisterMetaType<std::vector<QRCodeDetection>>("std::vector<QRCodeDetection>");
    QThread* scanThread = new QThread(&dialog);
    auto* scanWorker = new QRScanWorker(decoder);
    scanWorker->moveToThread(scanThread);
    connect(scanThread, &QThread::started, scanWorker, &QRScanWorker::start);
    connect(scanThread, &QThread::finished, scanWorker, &QObject::deleteLater);
    scanThread->start();
    bool isScanning = true;

    connect(scanWorker, &QRScanWorker::stableDetections,
            &dialog, [&, previewWidget, previewContainer, scanLine, hintLabel, scanWorker]
            (std::vector<QRCodeDetection> detections, int frameWidth, int frameHeight,
             const QImage& frameImage) {
        if (!isScanning) return;
            isScanning = false;
            QMetaObject::invokeMethod(scanWorker, "stop", Qt::QueuedConnection);
            scanLine->stop();

            if (detections.size() == 1) {
                auto* marker = new CircleMarker(previewWidget);
                const auto& det = detections[0];
                double scaleX = static_cast<double>(previewWidget->width()) / frameWidth;
                double scaleY = static_cast<double>(previewWidget->height()) / frameHeight;
                int centerX = static_cast<int>((det.rect.x + det.rect.width / 2.0) * scaleX);
                int centerY = static_cast<int>((det.rect.y + det.rect.height / 2.0) * scaleY);
                marker->move(centerX - 32, centerY - 32);
                marker->show();
                marker->raise();

                // 以 &dialog 为上下文：识别到关闭对话框之间这 500ms 内 dialog 可能已析构，
                // 而回调按引用捕获了栈上的 dialog 与 isScanning（缺上下文曾在实测中触发 SIGSEGV）
                QTimer::singleShot(500, &dialog,
                                   [&dialog, det, scanWorker, scanLine, marker, &isScanning]() {
                    marker->deleteLater();
                    QMessageBox msgBox(&dialog);
                    msgBox.setWindowTitle(QStringLiteral("识别结果"));
                    msgBox.setText(det.content);
                    msgBox.setStandardButtons(QMessageBox::Ok);
                    msgBox.exec();
                    isScanning = true;
                    QMetaObject::invokeMethod(scanWorker, "start", Qt::QueuedConnection);
                    scanLine->start(scanLine->parentWidget()->height());
                });
            } else {
                QPixmap frozenPixmap = QPixmap::fromImage(frameImage);

                auto* frozenImageLabel = new QLabel(previewContainer);
                frozenImageLabel->setGeometry(0, 0, previewWidget->width(), previewWidget->height());
                frozenImageLabel->setPixmap(frozenPixmap.scaled(previewWidget->width(),
                                                               previewWidget->height(),
                                                               Qt::KeepAspectRatio,
                                                               Qt::SmoothTransformation));
                frozenImageLabel->setScaledContents(true);
                frozenImageLabel->raise();
                frozenImageLabel->show();

                auto* overlayWidget = new QWidget(previewContainer);
                overlayWidget->setGeometry(0, 0, previewWidget->width(), previewWidget->height());
                overlayWidget->setStyleSheet(QStringLiteral("background:rgba(0, 0, 0, 0.5);"));
                overlayWidget->raise();
                overlayWidget->show();

                hintLabel->show();

                double scaleX = static_cast<double>(previewWidget->width()) / frameWidth;
                double scaleY = static_cast<double>(previewWidget->height()) / frameHeight;

                for (const auto& det : detections) {
                    int centerX = static_cast<int>((det.rect.x + det.rect.width / 2.0) * scaleX);
                    int centerY = static_cast<int>((det.rect.y + det.rect.height / 2.0) * scaleY);

                    auto* arrowBtn = new ArrowButton(overlayWidget);
                    arrowBtn->move(centerX - 36, centerY - 36);
                    arrowBtn->show();

                    connect(arrowBtn, &QPushButton::clicked, [&dialog, det, frozenImageLabel, overlayWidget,
                                                              hintLabel, scanWorker, scanLine, &isScanning]() {
                        QTimer::singleShot(500, &dialog,
                                           [&dialog, det, frozenImageLabel, overlayWidget,
                                            hintLabel, scanWorker, scanLine, &isScanning]() {
                            QMessageBox msgBox(&dialog);
                            msgBox.setWindowTitle(QStringLiteral("识别结果"));
                            msgBox.setText(det.content);
                            msgBox.setStandardButtons(QMessageBox::Ok);
                            msgBox.exec();
                            if (frozenImageLabel) frozenImageLabel->deleteLater();
                            if (overlayWidget) overlayWidget->deleteLater();
                            hintLabel->hide();
                            isScanning = true;
                            QMetaObject::invokeMethod(scanWorker, "start", Qt::QueuedConnection);
                            scanLine->start(scanLine->parentWidget()->height());
                        });
                    });
                }
            }
    });

    dialog.exec();

    // 收尾顺序不能调：先停扫码线程（它还在按 100ms 读 decoder 的帧指针），
    // 再停并删 decoder。反过来会让 worker 摸到已释放的 dst_bufs。
    // BlockingQueued 是为了确保返回时本轮 scanOnce 已经跑完，不是"投递了就算停"。
    if (scanThread->isRunning()) {
        QMetaObject::invokeMethod(scanWorker, "stop", Qt::BlockingQueuedConnection);
    }
    scanThread->quit();
    scanThread->wait();
    delete scanThread;   // 虽以 dialog 为 parent，但此处 dialog 还没析构，显式删可让线程立刻停
    decoder->stop();     // stop() 会把 capturePtr_ 置空，之后取帧一律拿到 nullptr
    delete decoder;
#endif // HAS_ZBAR
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
    auto* decoder = new FFmpegVideoDecoder();
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
