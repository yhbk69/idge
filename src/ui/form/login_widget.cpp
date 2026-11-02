#include "login_widget.h"

#include "construction_session.h"
#include "qr_decoder.h"        // unavailableReason()：状态行要说清"这台构建根本没编进 ZBar"
#include "qr_scan_dialog.h"
#include "theme.h"

#include <QByteArray>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

namespace {

// 大按钮统一口径：T1 档高度由 blacksoft.css 给不了（本页控件是代码建的，
// 不吃 [nav="top"] 那条选择器），所以这里显式写死 72px + 18px 字。
QString bigButtonQss(const char *bg, const char *fg, const char *pressed)
{
    return QString(
               "QPushButton { background:%1; color:%2; border:none; border-radius:10px;"
               "font-size:%3px; font-weight:600; padding:14px 24px; }"
               "QPushButton:hover { background:%4; }"
               "QPushButton:pressed { background:%5; }")
        .arg(bg, fg)
        .arg(theme::FS_NAV_TOUCH)
        .arg(QString::fromLatin1(bg))   // 登录页按钮不做 hover 变色，保持静态大色块
        .arg(pressed);
}

void applyBigButton(QPushButton *button)
{
    button->setMinimumHeight(72);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setCursor(Qt::PointingHandCursor);
}

// 右侧按钮列的宽度：480px ≈ 54mm，竖排三颗在这个宽度上文字不挤、命中区也够；
// 再宽就要吃掉左侧品牌块的空间，1920 宽下两边都不好看。
constexpr int kButtonColumnWidth = 480;

} // namespace

LoginWidget::LoginWidget(QWidget *parent) : QWidget(parent)
{
    auto *root = new QHBoxLayout(this);
    root->setContentsMargins(64, 48, 64, 48);
    root->setSpacing(56);

    // ── 左：设备身份 + 状态 ─────────────────────────────────
    auto *left = new QVBoxLayout;
    left->setSpacing(14);
    left->addStretch(1);

    auto *kicker = new QLabel(QString::fromUtf8("IDGE"), this);
    kicker->setStyleSheet(QString("QLabel { color:%1; font-size:%2px; font-weight:700; }")
                              .arg(theme::ACCENT)
                              .arg(theme::FS_PAGE));
    left->addWidget(kicker);

    // 品牌块：与顶部标题同源文案（frmmain.cpp 里 labTitle 写的是"施工行为监测与分析"）。
    // theme 字阶最高一档是 FS_KPI=30，落地页要能隔着一两米认出这是哪台设备，
    // 所以这里按 52px 写死——它不是通用文字档，别处不要照抄。
    auto *brand = new QLabel(QString::fromUtf8("施工行为监测与分析"), this);
    brand->setStyleSheet(QString("QLabel { color:%1; font-size:52px; font-weight:700; }")
                             .arg(theme::TEXT));
    left->addWidget(brand);

    auto *tagline = new QLabel(
        QString::fromUtf8("边缘智能视频分析终端 · 登录成功后才开始视频检测与报警"), this);
    tagline->setWordWrap(true);
    tagline->setStyleSheet(theme::text(theme::TEXT_MUTED, theme::FS_BODY));
    left->addWidget(tagline);

    left->addSpacing(24);

    statusLabel_ = new QLabel(this);
    statusLabel_->setWordWrap(true);
    statusLabel_->setTextInteractionFlags(Qt::NoTextInteraction);
    left->addWidget(statusLabel_);

    left->addStretch(2);

    // ── 右：三个操作，竖排 ─────────────────────────────────
    auto *rightColumn = new QWidget(this);
    rightColumn->setFixedWidth(kButtonColumnWidth);
    auto *right = new QVBoxLayout(rightColumn);
    right->setContentsMargins(0, 0, 0, 0);
    right->setSpacing(20);
    right->addStretch(1);

    scanButton_ = new QPushButton(QString::fromUtf8("扫码登录"), rightColumn);
    applyBigButton(scanButton_);
    scanButton_->setStyleSheet(bigButtonQss(theme::ACCENT, theme::BG, theme::BTN_SECONDARY_HOVER));
    connect(scanButton_, &QPushButton::clicked, this, &LoginWidget::onScanClicked);

    testButton_ = new QPushButton(QString::fromUtf8("测试登录"), rightColumn);
    applyBigButton(testButton_);
    testButton_->setStyleSheet(bigButtonQss(theme::PANEL, theme::TEXT, theme::HOVER));
    connect(testButton_, &QPushButton::clicked, this, &LoginWidget::onTestLoginClicked);

    exitButton_ = new QPushButton(QString::fromUtf8("退出程序"), rightColumn);
    applyBigButton(exitButton_);
    exitButton_->setStyleSheet(bigButtonQss(theme::DANGER, "#FFFFFF", theme::DANGER_PRESSED));
    connect(exitButton_, &QPushButton::clicked, this, &LoginWidget::exitProgramRequested);

    right->addWidget(scanButton_);
    right->addWidget(testButton_);
    right->addWidget(exitButton_);
    right->addStretch(1);

    root->addLayout(left, 1);
    root->addWidget(rightColumn, 0);

    resetStatus();
}

void LoginWidget::resetStatus()
{
    if (!QrScanDialog::available()) {
        setStatus(qrcode::unavailableReason(), true);
        return;
    }
    setStatus(QString::fromUtf8("未登录：请扫描现场施工二维码"), false);
}

void LoginWidget::setStatus(const QString &text, bool error)
{
    statusLabel_->setText(text);
    statusLabel_->setStyleSheet(
        theme::text(error ? theme::DANGER : theme::TEXT_MUTED, theme::FS_CARD, error));
}

void LoginWidget::onScanClicked()
{
    QString content;
    QString error;
    if (!QrScanDialog::scan(this, &content, &error)) {
        // error 为空 = 用户自己关掉了预览窗口，不是故障：状态行回到初始提示即可
        setStatus(error.isEmpty() ? QString::fromUtf8("未登录：请扫描现场施工二维码") : error,
                  !error.isEmpty());
        return;
    }

    if (session::ConstructionSession::instance().startLogin(content, &error)) {
        qInfo() << "Login succeeded by QR scan:"
                << session::ConstructionSession::instance().summary();
        emit loginSucceeded();
        return;
    }

    qWarning() << "QR scan rejected:" << error;
    setStatus(error, true);
    emit loginFailed(error);
}

bool LoginWidget::loadFakePayload(QString *jsonText, QString *err)
{
    // 候选路径按"最可能"排序，**必须多条**：RuntimePaths 全是相对 CWD 的，
    // 经 run.sh 启动时 CWD 是 build/build_rk3588_linux，仓库根下的 plan/ 根本不在
    // 相对路径上；而部署机上 plan/ 既不随克隆分发、也不该分发（伪造信息不进仓）。
    // data/ 那条是部署机上真正可用的落点：data/ 是运行时就存在的数据目录。
    const QString fileName = QStringLiteral("/construction_qr.json");
    const QString cwd = QDir::currentPath();
    const QStringList candidates = {
        qEnvironmentVariable("IDGE_PLAN_DIR") + fileName,
        cwd + QStringLiteral("/plan") + fileName,
        cwd + QStringLiteral("/../plan") + fileName,
        cwd + QStringLiteral("/../../plan") + fileName,
        cwd + QStringLiteral("/../../../plan") + fileName,
        cwd + QStringLiteral("/data") + fileName,
    };

    QStringList tried;
    for (const QString &raw : candidates) {
        if (raw.isEmpty())
            continue;   // IDGE_PLAN_DIR 未设置时那条是空路径，不算"试过"
        const QString path = QDir::cleanPath(raw);
        tried << path;
        QFile file(path);
        if (!file.exists())
            continue;
        if (!file.open(QIODevice::ReadOnly)) {
            if (err) {
                *err = QString::fromUtf8("测试信息文件无法读取：%1\n(%2)")
                           .arg(path, file.errorString());
            }
            return false;
        }
        const QByteArray data = file.readAll();
        file.close();
        if (data.trimmed().isEmpty()) {
            if (err) *err = QString::fromUtf8("测试信息文件是空的：%1").arg(path);
            return false;
        }
        *jsonText = QString::fromUtf8(data);
        qInfo() << "Test login payload loaded from" << path;
        return true;
    }

    // 一个都没找到：把每个试过的路径都列出来，现场照着放文件就行，绝不静默也不崩
    if (err) {
        *err = QString::fromUtf8("找不到测试登录信息文件，已尝试：\n%1")
                   .arg(tried.join(QLatin1Char('\n')));
    }
    qWarning() << "Test login payload not found; tried:" << tried;
    return false;
}

void LoginWidget::onTestLoginClicked()
{
    QString jsonText;
    QString error;
    if (!loadFakePayload(&jsonText, &error)) {
        qWarning() << "Test login failed:" << error;
        setStatus(error, true);
        emit loginFailed(error);
        return;
    }

    // 与扫码**完全同一条**路径：不许有第二套解析/校验分支
    if (session::ConstructionSession::instance().startLogin(jsonText, &error)) {
        qInfo() << "Login succeeded by test payload:"
                << session::ConstructionSession::instance().summary();
        emit loginSucceeded();
        return;
    }

    qWarning() << "Test login payload rejected:" << error;
    setStatus(error, true);
    emit loginFailed(error);
}
