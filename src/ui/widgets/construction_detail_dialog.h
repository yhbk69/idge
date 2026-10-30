#pragma once

// ============================================================================
// construction_detail_dialog.h - 「当前登录的施工信息」详情框
// ============================================================================
//
// 入口：标题栏右上角那颗登录人角标（frmMain::sessionBadge_）。
// 职责：把此刻会话里的施工信息整份列出来，让岗位上的人不翻报警详情就能确认
//       "现在系统记的是哪一班"。换班没退出登录时，这个确认动作是唯一的补救口。
//
// 只读、无信号无槽（所以本文件不需要 moc）；打开时读一次 snapshot()，
// 之后会话即使变了也不回查——框里显示的就是它打开那一刻的登录态。
//
// 字段渲染走 ConstructionInfo::fields()，与报警详情框同一个口径：
// 协议加字段时这里自动多一行，不需要改本文件。
// ============================================================================

#include <QDialog>

class ConstructionDetailDialog : public QDialog
{
public:
    explicit ConstructionDetailDialog(QWidget *parent = nullptr);

private:
    void setupUi();
};
