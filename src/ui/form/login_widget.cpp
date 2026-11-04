/**
 * @file login_widget.cpp
 * @brief 扫码登录页实现（主界面放行前的门；头文件的口径与版面说明见 login_widget.h）
 *
 * 本页只做三件事：把二维码原文（或伪造 JSON）交给 ConstructionSession::startLogin()、
 * 把结果作为信号报给 frmMain、把失败原因显示在状态行。
 * 不解析字段、不判合法性、不存会话——那些全在 src/biz/session，界面上不存第二套规则。
 *
 * 线程与设备占用（本页存在的前提）：
 *   - 全部代码在 GUI 线程跑。QrScanDialog::scan() 是**阻塞式模态**（内部 exec），
 *     扫码期间主窗事件循环仍在，但本页不再往下走；实时解码在解码线程，
 *     结果经队列连接回来看（见 src/ui/widgets/qr_scan_dialog.cpp）。
 *   - 本页**不常驻摄像头**：设备节点只在扫码窗格打开期间被持有。停在登录页时
 *     进程的 /dev/video* 句柄数为 0（板端实测）——这是"门挡着的时候不碰摄像头"
 *     能成立的前提，也避免与智能识别页抢同一个 /dev/videoNN。
 *   - 「测试登录」与扫码走**完全同一条** startLogin(qrText) 路径，没有第二套解析分支；
 *     测试数据（plan/construction_qr.json）不进仓，多候选路径的找法见 loadFakePayload。
 */
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

// 统一给三颗大按钮上触屏尺寸。72 = T1 档总高（≈8.1mm，岗位戴手套也按得到，
// 与 theme::TOUCH_MAIN_H 同值；本页这行是改造前写的，值一致就没换常量）。
// 只给**最小高度** + 横向 Expanding，不 setFixedSize：宽度由按钮列那一层
// （kButtonColumnWidth）定，这里写死就会和列宽互相顶；高度用 minimum 是因为
// QSS 的 padding 会把内容顶高，固定高反而可能截字。
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

/**
 * @brief 构造登录页：左品牌块 + 右三颗竖排大按钮（GUI 线程，纯建控件，不起定时器）
 *
 * 信号出口只有 loginSucceeded / loginFailed / exitProgramRequested 三条，
 * 本页不认识 frmMain 的任何内部状态——frmMain 把它 insertWidget 成 stackedWidget
 * 的第 0 页并接这三条线（见 frmmain.cpp 的 initNewPages）。
 * 末尾必调 resetStatus()：新构造出来的页也要按"本构建有没有编进 ZBar"决定状态行，
 * 否则缺 ZBar 的构建上会显示一句"请扫描现场施工二维码"这种做不到的事。
 * 刻意不做的事：不放常驻摄像头预览（会一直占着 /dev/videoNN，见文件头）。
 */
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

/**
 * @brief 把状态行复位到初始提示（frmMain 每次合门时调一次）
 *
 * 为什么必须复位：状态行是本页唯一的错误出口，不清的话新来的班次看到的是上一班
 * "缺字段"的红字，会以为是自己的码坏了。
 * 优先级：先判 QrScanDialog::available()（编译期没 HAS_ZBAR 时扫码器根本不存在，
 * 提示要说清"这台构建没编进 ZBar"，让人去查构建而不是去查摄像头），
 * 不可用就直接显示 unavailableReason() 并停在错误态。
 */
void LoginWidget::resetStatus()
{
    if (!QrScanDialog::available()) {
        setStatus(qrcode::unavailableReason(), true);
        return;
    }
    setStatus(QString::fromUtf8("未登录：请扫描现场施工二维码"), false);
}

/**
 * @brief 写状态行
 * @param text  要显示的文本（可以是多行；statusLabel_ 开了 wordWrap）
 * @param error true=DANGER 色 + 加粗，false=TEXT_MUTED 常规
 * 空串是允许的（会把状态行清空），但正常流程不要这么用：本页一屏只有这一行提示，
 * 空白会让人以为按钮点不动。
 */
void LoginWidget::setStatus(const QString &text, bool error)
{
    statusLabel_->setText(text);
    statusLabel_->setStyleSheet(
        theme::text(error ? theme::DANGER : theme::TEXT_MUTED, theme::FS_CARD, error));
}

/**
 * @brief 「扫码登录」：拉起全屏扫码窗格 → 解出原文 → 交 startLogin()
 *
 * QrScanDialog::scan() 是**阻塞式模态**，返回 false 有两种情况，必须分开处理：
 *   - error 为空 = 用户自己关掉了窗格（不是故障）→ 状态行回到初始提示，
 *     不发 loginFailed（发了会在 frmMain 日志里留一条"登录失败：（空）"的假事故）；
 *   - error 非空 = 真的没解出来/设备打不开 → 显示错误 + 发 loginFailed(reason)。
 * 成功路径：startLogin 已经把会话写好（归属从这一刻起定死），这里只发信号，
 * 开流/亮导航都是 frmMain::onLoginSucceeded 的事。
 * 现场口径：验扫码只认真摄像头对真码；要喂假数据请用「测试登录」（同一条 startLogin）。
 */
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

/**
 * @brief 读「测试登录」的伪造施工信息
 * @param jsonText [out] 文件原文；**不可为空指针**（命中时直接解引用写入）
 * @param err      [out] 失败原因，可为空指针（内部每处都判了 if (err)）
 * @return true = 已把原文写进 jsonText；false = 没读到，原因在 err 里
 *
 * 失败有三类，提示各不相同（现场排障全靠这一行，绝不静默）：
 *   文件不存在（列出**每个试过的路径**）、存在但打不开（带 QFile::errorString）、
 *   存在但内容空白（"测试信息文件是空的"）。
 * 中途某个候选路径存在却读失败就**直接返回**，不再试后面的路径——
 * 这是有意的：空文件/权限问题多半是人为放错位置，继续往下找只会掩盖它。
 */
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

/**
 * @brief 「测试登录」：读伪造 JSON → 走与扫码**同一条** startLogin()
 *
 * 存在的意义是"没有真码也能验会话链路"，不是一条平行实现：解析/校验/落会话
 * 全部同一个 ConstructionSession::startLogin(qrText,&err)，所以它通过就等于扫码通过。
 * 失败一律 setStatus(error,true) + emit loginFailed(reason)，与扫码路径同一套出口，
 * frmMain 侧不需要区分是谁登的录（日志文案由 reason 自己带）。
 */
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
