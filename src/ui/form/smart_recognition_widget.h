#ifndef SMART_RECOGNITION_WIDGET_H
#define SMART_RECOGNITION_WIDGET_H

/**
 * @file smart_recognition_widget.h
 * @brief 智能识别页（二维码 + OCR 铭牌文字 + 人员点名 + 设备盘点）对外接口
 *
 * 页面是纯 GUI 线程控件，二维码/OCR 两条链路都在模态对话框期间才跑，
 * 关掉对话框即释放解码器，因此本头文件里除两个业务页请求信号外没有别的信号槽、
 * 也不需要停止接口（析构即收尾，见 .cpp 的 openQrScanner/openOcrScanner 末尾）。
 *
 * 二维码/OCR 流程一致：打开摄像头预览 → 拍照 → 确认照片 → 识别 → 弹出结果。
 * 人员点名/设备盘点只负责发起请求（rollCallRequested / equipmentRequested），
 * 服务与业务页面的生命周期在 frmMain，本页面不持有。
 *
 * 实现细节、线程模型与三条前提见 smart_recognition_widget.cpp 的文件头注释。
 */

#include <QWidget>
#include <memory>
#include <string>
#include <vector>
#include <opencv2/opencv.hpp>

class SmartRecognitionWidget : public QWidget {
    Q_OBJECT

public:
    /// workspace: 工作区根目录（ frmMain 传入），用于拼 model/ 与 3rdparty/ 路径
    /// rollCallAvailable / equipmentAvailable: 两个业务服务是否初始化成功，
    /// 决定对应卡片是可用还是置灰（frmMain 先初始化服务再构造本页）
    explicit SmartRecognitionWidget(const std::string& workspace,
                                    bool rollCallAvailable, bool equipmentAvailable,
                                    QWidget* parent = nullptr);
    ~SmartRecognitionWidget() override;

signals:
    /// 点击"人员点名"/"设备盘点"卡片：由 frmMain 拉起对应业务页面的模态对话框
    void rollCallRequested();
    void equipmentRequested();

private:
    /// 卡片按钮：accent 决定主色，enabled=false 时置灰且不响应点击（CardClickFilter）
    QWidget* createLargeButton(const QString& title, const QString& description,
                               const char* accent, bool enabled);
    /// det/rec 模型 + 两个外部 demo 是否都在（缺一即卡片显示"未部署"并短路）
    bool ocrModelsReady() const;
    // OCR 模型目录（det/rec .rknn + labels.txt），遵循 model/README.md 的模型库约定
    QString ocrDir() const;
    // OCR 外部可执行目录（第三方，见 3rdparty/ppocrv4/README.md）
    QString ocrBinDir() const;

    void openQrScanner();
    void openOcrScanner();

    // OCR 三步对话框：拍照 → 选区 → 结果
    void showOcrPhotoDialog();
    void showOcrRegionDialog(const QString& imagePath);
    void showOcrResultDialog(const QStringList& texts);

    // 以下两个都起 3rdparty/ppocrv4/bin 的 demo、解析 stdout，超时或失败返回空
    /// imagePath 为整幅拍照图；返回 det（DB）检出的文本框，坐标是该图的像素
    std::vector<cv::Rect> runPpocrDet(const QString& imagePath);
    /// 把 det 的零散文本框按行聚成可选区（用户点的是"一行字"不是一个字）
    std::vector<cv::Rect> smartGroupBoxes(const std::vector<cv::Rect>& boxes,
                                         int imageWidth, int imageHeight);
    /// 没有用户选择时的默认选区：离画面中心最近的那个框；det 为空则整幅
    cv::Rect selectDefaultRegion(const std::vector<cv::Rect>& regions,
                                int imageWidth, int imageHeight);
    /// imagePath 须是**已裁好的选区图**；返回各行文本（demo 打印的 score 被丢弃）
    QStringList runPpocrSystem(const QString& imagePath);

    std::string workspace_;
};

#endif // SMART_RECOGNITION_WIDGET_H
