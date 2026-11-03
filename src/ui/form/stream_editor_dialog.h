#ifndef STREAM_EDITOR_DIALOG_H
#define STREAM_EDITOR_DIALOG_H

/*
====================================================
系统名称: IDGE
模块名称: 视频流注册表编辑对话框（video.sources[]）
说明: 左边列出每条流，右边编辑选中那条的参数；确定后把改动写回 ConfigManager，
      并把"改过/新建/删过"的流 id 交回调用方，由它决定要不要重开相关通道。
      为什么做成"列表 + 表单"而不是一张 12 列的表：本机是 7 寸触屏，
      表格单元格的点击目标远小于 6mm 下限（见 src/ui/core/theme.h 的 TOUCH_FIELD_H）。
====================================================
*/

#include <QDialog>
#include <QList>
#include <QStringList>

#include "ConfigManager.h"   // VideoSourceCfg

class QListWidget;
class QLineEdit;
class QComboBox;
class QSpinBox;
class QCheckBox;
class QPushButton;

class StreamEditorDialog : public QDialog
{
    Q_OBJECT

public:
    explicit StreamEditorDialog(QWidget *parent = nullptr);

    /// 本次编辑动过哪些流 id（新建/修改/删除都算），调用方据此挑要重开的通道
    QStringList touchedSourceIds() const { return touched_; }

private slots:
    void loadStream(const QString &id);      ///< 列表选中变化 → 表单填这条流
    void addStream();                        ///< 新建一条（id 自动生成，避免撞车）
    void removeStream();                     ///< 删除选中条
    void browseUrl();                        ///< 给"地址"挑一个本地文件
    void onAccept();                         ///< 确定：落盘 + 收窗

private:
    void buildUi();
    /// @brief 重灌左侧列表并选中 selectId；不传/找不到时退到第一行，列表空则禁用右侧
    void refreshList(const QString &selectId = QString());
    QString currentId() const;               ///< 列表当前选中的 id，无选中返回空串
    void setFieldEnabled(bool on);           ///< 没选中任何条时右侧整片禁用
    /// @brief 把表单当前值写回 id 那条流（id 不在工作副本里则什么都不做）
    void applyToModel(const QString &id);

    QList<VideoSourceCfg> streams_;          ///< 工作副本：确定前不动 ConfigManager
    QStringList touched_;                    ///< 见 touchedSourceIds()

    QListWidget *list_ = nullptr;
    QLineEdit *idEdit_ = nullptr;
    QComboBox *typeCombo_ = nullptr;
    QLineEdit *urlEdit_ = nullptr;
    QSpinBox *widthSpin_ = nullptr;
    QSpinBox *heightSpin_ = nullptr;
    QSpinBox *fpsSpin_ = nullptr;
    QComboBox *inputFormatCombo_ = nullptr;
    QCheckBox *loopCheck_ = nullptr;
    QCheckBox *hwCheck_ = nullptr;
    QSpinBox *timeoutSpin_ = nullptr;
    QSpinBox *reconnectSpin_ = nullptr;
    QLineEdit *noteEdit_ = nullptr;
    QPushButton *browseBtn_ = nullptr;
};

#endif // STREAM_EDITOR_DIALOG_H
