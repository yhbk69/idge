#include "qr_scan_dialog.h"

#include "gl_video_widget.h"
#include "qr_decoder.h"
#include "theme.h"
#include "ConfigManager.h"
#include "ffmpeg_video_decoder.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QRect>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <atomic>
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

// 多码标记：冻结图上用来点选的圆点按钮
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
    void paintEvent(QPaintEvent *event) override {
        QPushButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(Qt::white, 5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        const int cx = width() / 2;
        const int cy = height() / 2;
        painter.drawLine(cx - 12, cy - 14, cx + 10, cy);
        painter.drawLine(cx + 10, cy, cx - 12, cy + 14);
    }
};

// 取景叠层：四角括号 + 上下扫动的扫描线，只用来表达"正在实时扫描"。
// 不带 Q_OBJECT（只重写 paintEvent，不需要信号槽），所以本文件不需要 moc。
class ScanOverlay : public QWidget {
public:
    explicit ScanOverlay(QWidget *parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
    }

    void setActive(bool active) {
        if (active_ == active)
            return;
        active_ = active;
        update();
    }

    // 由 GUI 线程的定时器驱动（见 runLiveScan 里的 sweepTimer）
    void advance() {
        if (!active_)
            return;
        phase_ += 0.018;
        if (phase_ > 1.0)
            phase_ -= 1.0;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        if (!active_)
            return;
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF area = QRectF(rect()).adjusted(24, 24, -24, -24);
        if (area.width() < 80 || area.height() < 80)
            return;

        painter.setPen(QPen(QColor(theme::ACCENT), 6, Qt::SolidLine, Qt::RoundCap));
        const qreal arm = qMin<qreal>(56.0, qMin(area.width(), area.height()) / 5.0);
        painter.drawLine(area.topLeft(), area.topLeft() + QPointF(arm, 0));
        painter.drawLine(area.topLeft(), area.topLeft() + QPointF(0, arm));
        painter.drawLine(area.topRight(), area.topRight() - QPointF(arm, 0));
        painter.drawLine(area.topRight(), area.topRight() + QPointF(0, arm));
        painter.drawLine(area.bottomLeft(), area.bottomLeft() + QPointF(arm, 0));
        painter.drawLine(area.bottomLeft(), area.bottomLeft() - QPointF(0, arm));
        painter.drawLine(area.bottomRight(), area.bottomRight() - QPointF(arm, 0));
        painter.drawLine(area.bottomRight(), area.bottomRight() - QPointF(0, arm));

        const qreal y = area.top() + phase_ * area.height();
        QColor transparent(QString::fromLatin1(theme::ACCENT));
        transparent.setAlpha(0);
        QColor core(QString::fromLatin1(theme::ACCENT));
        core.setAlpha(190);
        QLinearGradient grad(0, y - 5, 0, y + 5);
        grad.setColorAt(0.0, transparent);
        grad.setColorAt(0.5, core);
        grad.setColorAt(1.0, transparent);
        painter.fillRect(QRectF(area.left(), y - 5, area.width(), 10), QBrush(grad));
    }

private:
    bool active_ = true;
    double phase_ = 0.0;
};

// 预览控件被布局改尺寸时，把所有叠层同步到同样大小。
// QOpenGLWidget 没有 resize 信号，只能接 QEvent::Resize；同样不需要 moc。
class OverlayFollower : public QObject {
public:
    explicit OverlayFollower(QWidget *target)
        : QObject(target), target_(target) {}

    void follow(QWidget *overlay) {
        overlays_.push_back(overlay);
        overlay->resize(target_->size());
    }

protected:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (obj == target_ && event->type() == QEvent::Resize) {
            for (QWidget *overlay : overlays_)
                overlay->resize(target_->size());
        }
        return QObject::eventFilter(obj, event);
    }

private:
    QWidget *target_;
    std::vector<QWidget *> overlays_;
};

enum class Outcome { Confirmed, Cancelled };

// ZBar 限流：解码线程上每 250ms 最多解一帧。实测板上 CSI 摄像头预览约 7.7fps，
// 150ms 间隔时解一帧要吃掉三成多的帧预算、预览掉到 5.1fps，对"举着码等它认"这件事
// 来说不值：现场码是静止张贴的，250ms 的识别延迟肉眼看不出来，预览顺不顺却看得出来。
constexpr qint64 kScanIntervalMs = 250;

// 实时扫码窗格。返回 Confirmed 时 confirmedContent 是用户点确认那颗码的原文。
Outcome runLiveScan(QWidget *parent, const QString &confirmText,
                    QString *confirmedContent, QString *err)
{
    QDialog dialog(parent);
    dialog.setWindowTitle(QString::fromUtf8("扫描二维码"));
    dialog.resize(1000, 760);
    dialog.setStyleSheet(QStringLiteral("QDialog { background:%1; }").arg(theme::BG));

    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    auto *previewContainer = new QWidget(&dialog);
    // 视频留边用纯黑，非主题色板成员
    previewContainer->setStyleSheet(QStringLiteral("background:#000000;"));
    auto *previewLayout = new QVBoxLayout(previewContainer);
    previewLayout->setContentsMargins(0, 0, 0, 0);
    previewLayout->setSpacing(0);

    auto *previewWidget = new GLVideoWidget(previewContainer);
    previewWidget->setMinimumSize(960, 540);
    previewLayout->addWidget(previewWidget);
    root->addWidget(previewContainer, 1);

    OverlayFollower follower(previewWidget);
    previewWidget->installEventFilter(&follower);

    auto *frozenLabel = new QLabel(previewWidget);
    frozenLabel->setAlignment(Qt::AlignCenter);
    frozenLabel->hide();
    follower.follow(frozenLabel);

    auto *pickLayer = new QWidget(previewWidget);
    pickLayer->setStyleSheet(QStringLiteral("background:rgba(0, 0, 0, 0.45);"));
    pickLayer->hide();
    follower.follow(pickLayer);

    auto *scanOverlay = new ScanOverlay(previewWidget);
    follower.follow(scanOverlay);
    scanOverlay->show();
    scanOverlay->raise();

    // 信息区常驻（不随识别结果显隐）：窗格高度因此恒定，解出码时不会把对话框顶大
    auto *infoLabel = new QLabel(&dialog);
    infoLabel->setWordWrap(true);
    infoLabel->setTextInteractionFlags(Qt::NoTextInteraction);
    infoLabel->setMinimumHeight(104);
    infoLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    infoLabel->setContentsMargins(28, 14, 28, 14);
    infoLabel->setStyleSheet(QString("QLabel { color:%1; font-size:%2px; background:%3;"
                                     "border-top:2px solid %4; }")
                                 .arg(theme::TEXT)
                                 .arg(theme::FS_CARD)
                                 .arg(theme::PANEL, theme::BORDER));
    infoLabel->setText(QString::fromUtf8("正在扫描：把现场张贴的二维码放进画面中央，"
                                         "解出后这里会显示码内内容。"));
    root->addWidget(infoLabel);

    auto *buttonRow = new QWidget(&dialog);
    buttonRow->setStyleSheet(QStringLiteral("background:%1;").arg(theme::BG));
    auto *buttonLayout = new QHBoxLayout(buttonRow);
    buttonLayout->setContentsMargins(20, 16, 20, 20);
    buttonLayout->setSpacing(16);

    auto makeButton = [&](const QString &text, const char *bg, const char *fg,
                          const char *pressed, int stretch) {
        auto *button = new QPushButton(text, buttonRow);
        button->setMinimumHeight(72);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setCursor(Qt::PointingHandCursor);
        button->setStyleSheet(QString(
            "QPushButton { background:%1; color:%2; border:none; border-radius:10px;"
            "font-size:%3px; font-weight:600; }"
            "QPushButton:hover { background:%4; }"
            "QPushButton:pressed { background:%5; }"
            "QPushButton:disabled { background:%6; color:%7; }")
                                  .arg(QString::fromLatin1(bg), QString::fromLatin1(fg))
                                  // FS_NAV_TOUCH 是导航行专用档（theme.h 明写"别处不要引用"），
                                  // 这里要的是同一个 18px，取语义正确的 FS_CARD
                                  .arg(theme::FS_CARD)
                                  // 触屏没有悬停态，hover 保持静态大色块（与登录页同一口径）
                                  .arg(QString::fromLatin1(bg))
                                  .arg(QString::fromLatin1(pressed))
                                  .arg(QString::fromLatin1(theme::BORDER),
                                       QString::fromLatin1(theme::TEXT_MUTED)));
        buttonLayout->addWidget(button, stretch);
        return button;
    };

    auto *againBtn = makeButton(QString::fromUtf8("重新扫描"), theme::PANEL, theme::TEXT,
                                theme::HOVER, 1);
    auto *cancelBtn = makeButton(QString::fromUtf8("取消"), theme::FIELD, theme::TEXT,
                                 theme::HOVER, 1);
    auto *confirmBtn = makeButton(confirmText, theme::SUCCESS, "#FFFFFF",
                                 theme::SUCCESS_HOVER, 2);
    root->addWidget(buttonRow);

    confirmBtn->setEnabled(false);
    againBtn->setEnabled(false);   // 还没解出码时"重新扫描"无意义，先按住

    // ── 扫描状态机 ─────────────────────────────────────────────
    // paused：GUI 线程在解出码时置位、点「重新扫描」时复位；解码线程每帧读一次。
    // 旧版靠"先停扫描线程再停解码器"的收尾顺序规避竞态，现在解码直接挂在解码线程上，
    // 没有第二条线程可停，所以这个标志必须是 atomic。
    std::atomic_bool paused{false};
    QString selectedContent;
    std::vector<qrcode::QrHit> currentHits;
    // 限流节拍只有解码线程读改，且解码线程在 runLiveScan 返回前一定被 stop() join 完，
    // 所以放栈上按引用捕获是安全的（不能用 thread_local 兜：那样它跨两次开窗格活着的
    // 是同一个时钟，第二次开窗的头 150ms 会被判为"刚解过"而不出码）。
    QElapsedTimer scanClock;
    scanClock.start();
    qint64 lastPassMs = -kScanIntervalMs;

    auto resumeScanning = [&]() {
        // 上一轮的点选圆点要清掉：pickLayer 只是被隐藏，留着的话下次解出多码
        // 会叠上两批圆点，点下去选中的是已经作废的那一帧
        for (QPushButton *arrow : pickLayer->findChildren<QPushButton *>())
            arrow->deleteLater();
        pickLayer->hide();
        frozenLabel->hide();
        scanOverlay->setActive(true);
        scanOverlay->raise();
        confirmBtn->setEnabled(false);
        againBtn->setEnabled(false);
        selectedContent.clear();
        currentHits.clear();
        infoLabel->setText(QString::fromUtf8("正在扫描：把现场张贴的二维码放进画面中央，"
                                             "解出后这里会显示码内内容。"));
        paused.store(false, std::memory_order_release);
    };

    auto selectHit = [&](size_t index) {
        if (index >= currentHits.size())
            return;
        selectedContent = currentHits[index].content;
        confirmBtn->setEnabled(!selectedContent.isEmpty());
        if (currentHits.size() > 1) {
            infoLabel->setText(QString::fromUtf8("识别到 %1 个二维码，已选第 %2 个：\n%3")
                                   .arg(static_cast<int>(currentHits.size()))
                                   .arg(index + 1)
                                   .arg(selectedContent));
        } else {
            infoLabel->setText(QString::fromUtf8("码内内容：\n%1").arg(selectedContent));
        }
    };

    // 只在 GUI 线程执行（由解码线程通过 invokeMethod 投递过来）
    auto onHits = [&](const QImage &frame, const std::vector<qrcode::QrHit> &hits) {
        // 用户已点确认、但还有一帧的解码结果排在队列里（收尾时会被就地派发）：
        // 这时候再改选会把用户手挑的那颗码换成第一颗，归属就错了
        if (dialog.result() == QDialog::Accepted)
            return;
        scanOverlay->setActive(false);
        againBtn->setEnabled(true);

        const QSize fit = previewWidget->size();
        const QSizeF scaled = QSizeF(frame.size()).scaled(fit, Qt::KeepAspectRatio);
        frozenLabel->setPixmap(QPixmap::fromImage(frame).scaled(
            scaled.toSize(), Qt::KeepAspectRatio, Qt::SmoothTransformation));
        frozenLabel->show();
        frozenLabel->raise();

        currentHits = hits;
        // 冻结图在控件里居中，所以码位坐标要补上这个黑边偏移（比例对不上时才非零）
        const QPoint offset((fit.width() - scaled.width()) / 2.0,
                            (fit.height() - scaled.height()) / 2.0);
        const double scaleX = scaled.width() / frame.width();
        const double scaleY = scaled.height() / frame.height();
        auto placeAt = [&](QWidget *widget, const QRect &rect, int half) {
            const QPointF center(offset.x() + (rect.center().x()) * scaleX,
                                 offset.y() + (rect.center().y()) * scaleY);
            widget->move(QPointF(center.x() - half, center.y() - half).toPoint());
        };

        if (hits.size() > 1) {
            pickLayer->show();
            pickLayer->raise();
            for (size_t i = 0; i < hits.size(); ++i) {
                auto *arrow = new ArrowButton(pickLayer);
                placeAt(arrow, hits[i].rect, arrow->width() / 2);
                arrow->show();
                QObject::connect(arrow, &QPushButton::clicked, &dialog,
                                 [selectHit, i]() { selectHit(i); });
            }
        } else {
            pickLayer->hide();
        }
        selectHit(0);
    };

    QObject::connect(againBtn, &QPushButton::clicked, &dialog,
                     [&resumeScanning]() { resumeScanning(); });
    QObject::connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    QObject::connect(confirmBtn, &QPushButton::clicked, &dialog,
                     [&dialog, &selectedContent]() {
                         if (!selectedContent.isEmpty())
                             dialog.accept();
                     });

    // 扫描线动画：40ms 一跳，一整程约 2.2s。上下文对象必须是 dialog——回调按引用捕获了
    // dialog 的栈上成员，用户在动画间隔里关掉窗格时它们已经析构。
    QTimer sweepTimer;
    QObject::connect(&sweepTimer, &QTimer::timeout, &dialog,
                    [scanOverlay]() { scanOverlay->advance(); });
    sweepTimer.start(40);

    const QString cameraUrl = ConfigManager::instance().smartRecognitionCamera();
    // 只取画面帧给 ZBar，不参与级联推理：PreviewOnly 不加载任何 RKNN 模型
    auto *decoder = new FFmpegVideoDecoder(
        nullptr, FFmpegVideoDecoder::InferenceMode::PreviewOnly);
    QObject::connect(decoder, &FFmpegVideoDecoder::frameReady,
                     previewWidget, &GLVideoWidget::onFrameReady, Qt::DirectConnection);

    // 解码放在解码线程做，而不是 GUI 线程：ZBar 解一帧是几十毫秒级 CPU 活，
    // 留在 GUI 线程会直接卡住画面（智能识别页是在四路都在跑的时候进来的）。
    // 用 frameReady 当"来新帧了"的节拍并限流，比自建扫描线程少一条线程要收尾；
    // 指针就在生产它的线程上读，双缓冲的覆盖竞态天然不存在。
    auto scanConn = QObject::connect(
        decoder, &FFmpegVideoDecoder::frameReady, decoder,
        [&dialog, decoder, &paused, &onHits, &scanClock, &lastPassMs]() {
            if (paused.load(std::memory_order_acquire))
                return;
            const qint64 now = scanClock.elapsed();
            if (now - lastPassMs < kScanIntervalMs)
                return;
            lastPassMs = now;

            const uint8_t *ptr = decoder->captureCurrentFramePtr();
            int w = 0, h = 0, stride = 0;
            if (!ptr || !decoder->getCurrentFrameInfo(w, h, stride))
                return;

            std::vector<qrcode::QrHit> hits;
            QString decodeError;
            if (!qrcode::scanQrFromRGBA(ptr, w, h, stride, &hits, &decodeError))
                return;
            hits.erase(std::remove_if(hits.begin(), hits.end(),
                                      [](const qrcode::QrHit &hit) {
                                          return hit.content.isEmpty();
                                      }),
                       hits.end());
            if (hits.empty())
                return;

            // 冻结帧必须在解码线程拷走：captureCurrentFramePtr() 指向的槽约两帧后就被覆盖
            const QImage frozen = captureFrameAsImage(decoder);
            if (frozen.isNull())
                return;

            paused.store(true, std::memory_order_release);
            QMetaObject::invokeMethod(
                &dialog, [&onHits, frozen, hits]() { onHits(frozen, hits); },
                Qt::QueuedConnection);
        },
        Qt::DirectConnection);

    decoder->start(cameraUrl);
    dialog.exec();

    // 收尾顺序不能调：先断开扫描回调，再 stop()（它会 join 解码线程，
    // 返回后 frameReady 不可能再发），最后把已经投递但还没派发的 onHits 就地跑完——
    // dialog 与它捕获的那些栈对象此刻仍然有效。放到函数返回后再派发就是野指针。
    QObject::disconnect(scanConn);
    paused.store(true, std::memory_order_release);
    decoder->stop();
    QCoreApplication::sendPostedEvents(&dialog, QEvent::MetaCall);
    delete decoder;

    if (dialog.result() != QDialog::Accepted || selectedContent.isEmpty()) {
        if (dialog.result() == QDialog::Accepted && selectedContent.isEmpty() && err)
            *err = QString::fromUtf8("没有选中任何二维码内容");
        return Outcome::Cancelled;
    }
    *confirmedContent = selectedContent;
    return Outcome::Confirmed;
}

} // namespace

bool QrScanDialog::available()
{
    return qrcode::decodeAvailable();
}

bool QrScanDialog::scan(QWidget *parent, QString *content, QString *err)
{
    if (!qrcode::decodeAvailable()) {
        // 没有 ZBar 时连预览都不该开：开了也解不出码，白占摄像头
        if (err) *err = qrcode::unavailableReason();
        return false;
    }
    QString confirmed;
    if (runLiveScan(parent, QString::fromUtf8("确认登录"), &confirmed, err) != Outcome::Confirmed)
        return false;
    if (content) *content = confirmed;
    return true;
}

void QrScanDialog::openFullScanner(QWidget *parent)
{
    if (!qrcode::decodeAvailable()) {
        QMessageBox::information(parent, QString::fromUtf8("不可用"),
                                 qrcode::unavailableReason());
        return;
    }
    QString content;
    QString error;
    // 窗格里已经显示过码内内容并由用户确认，这里只需要在真出错时打扰一句
    if (runLiveScan(parent, QString::fromUtf8("确认"), &content, &error) == Outcome::Cancelled
        && !error.isEmpty()) {
        QMessageBox::warning(parent, QString::fromUtf8("错误"), error);
    }
}
