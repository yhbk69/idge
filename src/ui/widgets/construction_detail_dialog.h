#pragma once

// ============================================================================
// construction_detail_dialog.h - 「当前登录的施工信息」详情框
// ============================================================================
//
// 入口：标题栏右上角那颗登录人角标（frmMain::sessionBadge_）。
// 职责：把此刻会话里的施工信息整份列出来，让岗位上的人不翻报警详情就能确认
//       "现在系统记的是哪一班"。换班没退出登录时，这个确认动作是唯一的补救口。
//
// 唯一的动作是底部那颗「退出登录」：它只记下"用户要退"并把框关掉，
// 真正的登出（确认框 → 停流 → 清限流窗口 → 合门）由 frmMain::onLogoutRequested() 做。
// 不在这里直接调它，是因为那一步自己还要弹一个确认框 —— 套在本框的模态循环里
// 就成了两层模态，确认框会压在详情框下面，岗位上按不到。调用方口径：
//     ConstructionDetailDialog dlg(this);
//     dlg.exec();
//     if (dlg.logoutRequested()) onLogoutRequested();
//
// 打开时读一次 snapshot()，之后会话即使变了也不回查——框里显示的就是它打开那一刻的
// 登录态。字段渲染走 ConstructionInfo::fields()，与报警详情框同一个口径：
// 协议加字段时这里自动多一行，不需要改本文件。
// ============================================================================

#include <QDialog>

class ConstructionDetailDialog : public QDialog
{
public:
    explicit ConstructionDetailDialog(QWidget *parent = nullptr);

    // 用户是否按了「退出登录」（按了才会顺带把本框关掉）
    bool logoutRequested() const { return logoutRequested_; }

private:
    void setupUi();

    bool logoutRequested_ = false;
};
