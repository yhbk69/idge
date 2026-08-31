#ifndef PLAYER_WIDGET_H
#define PLAYER_WIDGET_H


#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include "gl_video_widget.h"
#include "ffmpeg_video_decoder.h"


class PlayerWidget : public QWidget {
    Q_OBJECT

public:
    explicit PlayerWidget(QWidget* parent = nullptr);
    ~PlayerWidget();
    void open(std::string url);

private:

    GLVideoWidget* video_widget_;
    QLabel* overlayLabel;
    FFmpegVideoDecoder* decoder_;

     //工具栏单击
private slots:
    //处理按钮单击
    void btnClicked();

Q_SIGNALS:    
    void btnClicked(const QString &objName);

};

#endif