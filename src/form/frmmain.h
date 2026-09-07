#ifndef FRMMAIN_H
#define FRMMAIN_H

#include <QWidget>
#include <QTextCursor>
#include <QTextCharFormat>

class QAbstractButton;

namespace Ui {
class frmMain;
}

class frmMain : public QWidget
{
    Q_OBJECT

public:
    explicit frmMain(QWidget *parent = 0);
    ~frmMain();

protected:
    bool eventFilter(QObject *watched, QEvent *event);

private:
    Ui::frmMain *ui;

    QList<int> iconsMain;
    QList<QAbstractButton *> btnsMain;

    QList<int> iconsConfig;
    QList<QAbstractButton *> btnsConfig;

private:
    //根据QSS样式获取对应颜色值
    QString borderColor;
    QString normalBgColor;
    QString darkBgColor;
    QString normalTextColor;
    QString darkTextColor;
    
    void getQssColor(const QString &qss, const QString &flag, QString &color);
    void getQssColor(const QString &qss, QString &textColor,
                     QString &panelColor, QString &borderColor,
                     QString &normalColorStart, QString &normalColorEnd,
                     QString &darkColorStart, QString &darkColorEnd,
                     QString &highColor);

    // 日志功能（参考 rknn_Multithread 项目）
    QString currentTimestamp();
    void log(const QString &category, const QString &message);
    void logWithColor(const QString &category, const QString &message, const QColor &color);

private slots:
    void initForm();
    void initStyle();
    void buttonClick();
    void initLeftMain();
    void initLeftConfig();
    void leftMainClick();
    void leftConfigClick();
    void systemExit();
    void initDebugPage();
    void appendLog(const QString &msg);
    void on_btnBrowseVideo_clicked();
    void on_btnBrowseModel_clicked();
    void on_btnBrowseLabel_clicked();
    void on_spinBoxConfThresh_valueChanged(double value);
    void on_spinBoxNmsThresh_valueChanged(double value);

private slots:
    void on_btnMenu_Min_clicked();
    void on_btnMenu_Max_clicked();
    void on_btnMenu_Close_clicked();
    void on_pageRoll_customContextMenuRequested(const QPoint &pos);
};

#endif // FRMMAIN_H
