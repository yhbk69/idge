#ifndef PLAYER_WIDGET_H
#define PLAYER_WIDGET_H


#include <QPushButton>
#include <QVBoxLayout>
#include <QFileDialog>
#include <QLabel>
#include "gl_video_widget.h"
#include "ffmpeg_video_decoder.h"

/* 
====================================================
作用：播放器界面组件 - 提供视频播放的用户界面
说明：整合了视频显示、解码器和控制按钮
====================================================
*/
class PlayerWidget : public QWidget {
    Q_OBJECT

public:
    /* 
    ====================================================
    作用：构造函数
    说明：初始化播放器界面，创建必要的UI组件
    参数：parent - 父窗口指针
    ====================================================
    */
    explicit PlayerWidget(QWidget* parent = nullptr);

    /* 
    ====================================================
    作用：析构函数
    说明：释放播放器资源
    ====================================================
    */
    ~PlayerWidget();

    /* 
    ====================================================
    作用：打开视频源
    说明：开始解码并显示指定的视频流
    参数：url - 视频源地址（文件路径或网络流）
          channel - 频道号（用于多画面显示）
    ====================================================
    */
    void open(std::string url, int channel = 0);

    /* 
    ====================================================
    作用：停止解码器
    说明：安全停止视频解码，释放相关资源
    ====================================================
    */
    void stopDecoder();

    /* 
    ====================================================
    作用：获取解码器指针
    说明：提供对内部解码器的访问，用于外部控制
    返回值：FFmpegVideoDecoder指针
    ====================================================
    */
    FFmpegVideoDecoder* decoder() const { return decoder_; }

public:
    // 设置放大状态
    void setExpanded(bool expanded);
    bool isExpanded() const { return expanded_; }

protected:
    void resizeEvent(QResizeEvent *event) override;

private:
    void setupExpandButton();

    GLVideoWidget* video_widget_;  // OpenGL视频显示组件
    QLabel* overlayLabel;  // 覆盖层标签（用于显示叠加信息）
    FFmpegVideoDecoder* decoder_;  // FFmpeg视频解码器
    QPushButton* expandBtn_;  // 放大/缩小按钮
    bool expanded_ = false;  // 当前是否放大状态

private slots:
    void onExpandClicked();
    /* 
    ====================================================
    作用：处理按钮点击槽函数
    说明：响应用户界面按钮点击事件
    ====================================================
    */
    void btnClicked();

Q_SIGNALS:    
    /* 
    ====================================================
    作用：按钮点击信号
    说明：发送按钮点击事件，通知其他组件
    参数：objName - 被点击按钮的对象名称
    ====================================================
    */
    void btnClicked(const QString &objName);

};

#endif