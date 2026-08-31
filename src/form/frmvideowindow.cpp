#pragma execution_character_set("utf-8")

#include "frmvideowindow.h"
#include "ui_frmvideowindow.h"
#include <QTimer>
#include "player_widget.h"

frmVideoWindow::frmVideoWindow(QWidget *parent) : QWidget(parent), ui(new Ui::frmVideoWindow)
{
    ui->setupUi(this);
    this->initForm();
}

frmVideoWindow::~frmVideoWindow()
{
    delete ui->videoWindow1;
    delete ui->videoWindow2;
    delete ui->videoWindow3;
    delete ui->videoWindow4;
    delete ui;
}

void frmVideoWindow::initForm()
{
    // ui->videoWindow1->setFlowEnable(false);
    // ui->videoWindow2->setFlowEnable(false);
    // ui->videoWindow3->setFlowEnable(false);
    // ui->videoWindow4->setFlowEnable(false);

    connect(ui->videoWindow1, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow2, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow3, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    connect(ui->videoWindow4, SIGNAL(btnClicked(QString)), this, SLOT(btnClicked(QString)));
    
    // QTimer::singleShot(0, this, [this](){
    //     ui->videoWindow1->open(QString("/home/ubuntu/projects/dl_edge/192.mp4"));
    //     ui->videoWindow2->open(QString("/home/ubuntu/projects/dl_edge/192.mp4"));
    // });
    ui->videoWindow1->open("q3r");
    ui->videoWindow2->open("q3r");
    ui->videoWindow3->open("q3r");
    ui->videoWindow4->open("q3r");
}

void frmVideoWindow::btnClicked(const QString &objName)
{
    //VideoWindow *videoWindow = (VideoWindow *)sender();
    PlayerWidget *videoWindow = (PlayerWidget *)sender();
    // QString str = QString("当前单击了控件 %1 的按钮 %2").arg(videoWindow->objectName()).arg(objName);
    //GLWidget *videoWindow = (GLWidget *)sender();
    QString str = QString("当前单击了控件 %1 的按钮 %2").arg(videoWindow->objectName()).arg(objName);
    //qDebug() << TIMEMS << "paintEvent" << objectName();
    ui->label->setText(str);
}
