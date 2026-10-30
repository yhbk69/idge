#pragma once

// ============================================================================
// login_widget.h - 扫码登录页（主界面放行前的门）
// ============================================================================
//
// 职责：只做"拿到一份施工信息 JSON 文本 → 交给 ConstructionSession 校验登录"，
// 然后把结果作为信号报给 frmMain。页面本身不判定字段、不解析 JSON、不存会话，
// 所以将来字段格式改动不会牵到界面（改的地方只有 ConstructionInfo）。
//
// 两个登录入口走的是**同一条** ConstructionSession::startLogin(qrText) 路径：
//   扫码登录 → QrScanDialog::scan() 实时扫出码内原文（窗格打开即扫，无快门键）
//   测试登录 → 读 plan/construction_qr.json（伪造信息，多候选路径，见 .cpp）
// 没有第二套解析/校验分支，测试登录通过就等于扫码登录通过。
//
// 版面（2026-10-30 触屏复核后改）：**左品牌块 + 右操作列**两栏。
// 左边给设备身份（IDGE / 施工行为监测与分析）、登录状态行与取景说明，
// 右边只有三颗按钮并竖排。按钮都是 T1 档（min-height 72px ≈ 8.1mm，480px 宽 ≈ 54mm），
// 岗位戴手套也能按到；「退出程序」是全机唯一的退出通道之一（另一条是标题栏 ×），
// 顶部导航那颗现在叫「退出登录」，只回本页、不结束进程。
// 本页不放摄像头预览：预览在扫码窗格里，页面常驻会让门禁一直占着 /dev/video22。
//
// ============================================================================

#include <QWidget>

class QLabel;
class QPushButton;

class LoginWidget : public QWidget
{
    Q_OBJECT

public:
    explicit LoginWidget(QWidget *parent = nullptr);

    // 每次回到登录页（启动 / 退出登录后）调一次：把上一次的失败原因擦掉，
    // 否则新来的班次看到的是上一班"缺字段"的红字，会以为是自己的码坏了。
    void resetStatus();

signals:
    void loginSucceeded();                     ///< 会话已写好，frmMain 放行主界面并开流
    void loginFailed(const QString &reason);   ///< 已在页面状态行显示，这里只给日志留一份
    void exitProgramRequested();               ///< 走 frmMain::systemExit()（按所在屏居中）

private slots:
    void onScanClicked();
    void onTestLoginClicked();

private:
    void setStatus(const QString &text, bool error);
    // 伪造信息的落点：按候选路径顺序找第一个存在的文件，全找不到时
    // err 里把**每个试过的路径都列出来**（现场排障靠这行，绝不静默）
    static bool loadFakePayload(QString *jsonText, QString *err);

    QLabel *statusLabel_ = nullptr;
    QPushButton *scanButton_ = nullptr;
    QPushButton *testButton_ = nullptr;
    QPushButton *exitButton_ = nullptr;
};
