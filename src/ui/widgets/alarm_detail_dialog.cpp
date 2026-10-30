#include "alarm_detail_dialog.h"
#include "construction_session.h"
#include "qthelper.h"
#include "theme.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QMessageBox>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QPixmap>
#include <QScreen>
#include <QGuiApplication>
#include <QVector>

AlarmDetailDialog::AlarmDetailDialog(const AlarmRecord &alarm, QWidget *parent)
    : QDialog(parent), alarm_(alarm)
{
    setupUi();
    loadScreenshot();
}

void AlarmDetailDialog::setupUi()
{
    setWindowTitle("报警详情");
    setMinimumSize(480, 520);
    setStyleSheet(QString("background: %1; color: %2;").arg(theme::PANEL, theme::TEXT));

    QVBoxLayout *mainLay = new QVBoxLayout(this);
    mainLay->setContentsMargins(16, 16, 16, 16);
    mainLay->setSpacing(12);

    // 截图预览区域
    imageLabel_ = new QLabel();
    imageLabel_->setMinimumSize(440, 280);
    imageLabel_->setStyleSheet(QString("background: %1; border: 1px solid %2; border-radius: 4px;")
                                   .arg(theme::BG, theme::BORDER));
    imageLabel_->setAlignment(Qt::AlignCenter);
    imageLabel_->setText("无截图");
    mainLay->addWidget(imageLabel_);

    // 信息网格
    QGridLayout *infoLay = new QGridLayout();
    infoLay->setHorizontalSpacing(12);
    infoLay->setVerticalSpacing(8);

    auto addRow = [&](int row, const QString &label, const QString &value) {
        QLabel *lbl = new QLabel(label);
        lbl->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
        QLabel *val = new QLabel(value);
        val->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY, true));
        infoLay->addWidget(lbl, row, 0);
        infoLay->addWidget(val, row, 1);
        return val;
    };

    QDateTime dt = QDateTime::fromString(alarm_.alarmTime, Qt::ISODate);

    addRow(0, "时间:", dt.toString("yyyy-MM-dd HH:mm:ss"));
    addRow(1, "通道:", QString("通道 %1").arg(alarm_.channel + 1));
    addRow(2, "类别:", alarm_.className);
    addRow(3, "置信度:", QString("%1%").arg(alarm_.confidence * 100, 0, 'f', 1));
    addRow(4, "ID:", alarm_.id);
    statusLabel_ = addRow(5, "状态:", alarm_.status == "rectified" ? "已确认" : "未确认");
    if (alarm_.status != "rectified") {
        statusLabel_->setStyleSheet(theme::text(theme::WARNING, theme::FS_BODY, true));
    } else {
        statusLabel_->setStyleSheet(theme::text(theme::SUCCESS, theme::FS_BODY, true));
    }

    // 报警视频片段：硬编环形缓冲区在"报警后 2 秒"才落盘，所以打开得快时可能还没有。
    // 这里只报状态/路径，不做播放——裸流浏览器放不了，网页侧播同名的 .mp4 副本。
    QString clipText;
    if (alarm_.videoPath.isEmpty()) {
        clipText = "未生成";
    } else {
        const QFileInfo clip(alarm_.videoPath);
        if (!clip.exists()) {
            clipText = "已清理";
        } else {
            const QFileInfo webCopy(clip.absolutePath() + QStringLiteral("/")
                                    + clip.completeBaseName() + QStringLiteral(".mp4"));
            clipText = QString("%1（%2 KB）%3")
                .arg(clip.fileName())
                .arg(clip.size() / 1024)
                .arg(webCopy.exists() ? QStringLiteral(" · 网页副本 .mp4") : QString());
        }
    }
    addRow(6, "视频:", clipText);

    // 施工信息：按 JSON 里有什么就列什么，键→中文名与顺序由 ConstructionInfo::fields()
    // 给（与「施工详情」框同一个口径，见 src/biz/session/construction_session.h）。
    // 未登录时产生的报警（以及本轮之前的全部历史行）这里是空的 —— 不回填，
    // 历史上确实没有这份信息，显示成一行说明比留白或假数据诚实。
    const QVector<session::ConstructionField> construction =
        session::ConstructionInfo::fieldsFromJson(alarm_.constructionJson);
    int nextRow = 7;
    if (construction.isEmpty()) {
        QLabel *val = addRow(nextRow++, QString::fromUtf8("施工信息:"),
                             QString::fromUtf8("— （该报警产生时未登录，无施工信息）"));
        val->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
    } else {
        for (const session::ConstructionField &row : construction) {
            addRow(nextRow++, row.label + QStringLiteral("："), row.value);
        }
    }

    mainLay->addLayout(infoLay);

    mainLay->addStretch();

    // 按钮区域：确认误报 | 不是误报 | 取消
    QHBoxLayout *btnLay = new QHBoxLayout();
    btnLay->setSpacing(12);

    QPushButton *btnFalse = new QPushButton("确认误报");
    btnFalse->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::DANGER)
            .arg(theme::FS_BODY));
    connect(btnFalse, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkFalsePositive);
    btnLay->addWidget(btnFalse);

    QPushButton *btnNormal = new QPushButton("不是误报");
    btnNormal->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::SUCCESS)
            .arg(theme::FS_BODY));
    connect(btnNormal, &QPushButton::clicked, this, &AlarmDetailDialog::onMarkNormal);
    btnLay->addWidget(btnNormal);

    btnLay->addStretch();

    QPushButton *btnCancel = new QPushButton("取消");
    btnCancel->setStyleSheet(
        QString("QPushButton { color: %1; background: %2; border-radius: 4px; padding: 8px 20px; font-size:%3px; }"
                "QPushButton:hover { background: %2; }")
            .arg(theme::TEXT, theme::HOVER)
            .arg(theme::FS_BODY));
    connect(btnCancel, &QPushButton::clicked, this, &QDialog::reject);
    btnLay->addWidget(btnCancel);

    mainLay->addLayout(btnLay);
}

void AlarmDetailDialog::loadScreenshot()
{
    if (alarm_.imagePath.isEmpty() || !QFile::exists(alarm_.imagePath)) {
        imageLabel_->setText("无截图");
        return;
    }

    QPixmap pix(alarm_.imagePath);
    if (pix.isNull()) {
        imageLabel_->setText("截图加载失败");
        return;
    }

    QPixmap scaled = pix.scaled(imageLabel_->size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
    imageLabel_->setPixmap(scaled);
}

void AlarmDetailDialog::onMarkFalsePositive()
{
    QMessageBox box(QMessageBox::Question, tr("确认误报"), tr("确定将此报警标记为误报？\n标记后状态将变为误报。"),
                    QMessageBox::Ok | QMessageBox::Cancel, nullptr);
    box.setWindowFlags(box.windowFlags() | Qt::WindowStaysOnTopHint);
    QtHelper::localizeMessageBoxButtons(&box);
    box.adjustSize();

    // 按"本对话框所在屏"居中，不是 primaryScreen：双屏调试机上 primary 是外接屏，
    // 这个确认框会整块弹到没有触摸的那台显示器上，岗位上按不到。与 frmMain::systemExit 同一条口径。
    if (QScreen *screen = QGuiApplication::screenAt(geometry().center())) {
        QRect scr = screen->availableGeometry();
        box.move(scr.center() - box.rect().center());
    }
    box.raise();
    box.activateWindow();

    if (box.exec() == QMessageBox::Ok) {
        emit alarmMarkedFalsePositive(alarm_.id);
        accept();
    }
}

void AlarmDetailDialog::onMarkNormal()
{
    emit alarmAcknowledged(alarm_.id);
    accept();
}
