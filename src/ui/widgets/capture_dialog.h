#ifndef CAPTURE_DIALOG_H
#define CAPTURE_DIALOG_H

// ============================================================================
// capture_dialog.h - 处置取证的拍照框（只从实时摄像头拍，没有相册）
// ============================================================================
//
// 【所属模块】src/ui/widgets。被报警详情框的「处置」表单调起，exec() 模态使用。
// 【为什么只能拍照】2026-11-06 现场指定：整改照片要证明"人去现场处理过"，
//   从相册挑一张旧照片就等于伪造证据，所以这里**不提供**文件选择器，
//   连拔掉的 U 盘都不给读——与点名/盘点那个 PhotoSelectionDialog 的区别就在这一条。
// 【相机生命周期】构造即开流、析构即停流：对话框活着 = 相机被占着。
//   取流地址用 smartRecognition.camera（与 QR/OCR/点名同一台摄像头）。
// 【不跑检测】detector configs 传空，纯预览不画框：处置现场要的是"拍到那张图"，
//   而 NPU 正被四路检测流水線占着，这里再挂一个模型只会拖慢主链路。
// ============================================================================

#include <QDialog>
#include <QStringList>

class CameraPreviewDecoder;
class GLVideoWidget;
class QHBoxLayout;
class QLabel;
class QPushButton;
class QCloseEvent;

class CaptureDialog : public QDialog
{
    Q_OBJECT
public:
    // dir 是照片落盘目录（绝对路径，由调用方按 <媒体根>/dispose/<alarmId> 算好）；
    // maxCount 是张数上限（v1 §4.2 的 0~5 张），到量后快门自己变灰。
    explicit CaptureDialog(const QString &dir, int maxCount = 5, QWidget *parent = nullptr);
    ~CaptureDialog() override;

    /// 已拍照片的**绝对路径**，按拍摄顺序；取消时调用方不该用这个值
    QStringList photoPaths() const { return photos_; }

protected:
    // 没点"用这些照片"就关框（取消/ESC/✕）：本次拍的每一张都从盘上删掉。
    // 留着就是"库里有这条报警、磁盘上多几组没人认领的 jpg"，而 data/alarms/dispose/
    // 不归按天清理管（它不是日期目录），会一直攒到把卡写满。
    void closeEvent(QCloseEvent *event) override;
    void reject() override;

private slots:
    void onTakePhoto();
    // 解码线程落盘完成后经 QueuedConnection 回到 GUI 线程
    void onCaptured(const QString &path, const QImage &thumbnail);
    void onCameraError(const QString &message);

private:
    void setupUi();
    void startCamera();
    // 目录里第一个没用过的 disposeN.jpg（N 从 1 起）：同一报警可能分几次拍，
    // 不能从 1 重来把上次的覆盖掉。
    QString nextFilePath() const;
    // 没被采用就关框时，把本框写出去的每一张删掉（见 closeEvent 上方那段注释）。
    void discardIfNotAccepted();

    QString dir_;
    int maxCount_;
    QStringList photos_;
    QStringList takenFiles_;   ///< 本框写出去过的全部文件（含已被"删除"那颗清掉的）
    bool accepted_ = false;    ///< 点过"用这些照片"才保留

    CameraPreviewDecoder *decoder_ = nullptr;
    GLVideoWidget *preview_ = nullptr;
    QLabel *shutterOverlay_ = nullptr;
    QWidget *thumbArea_ = nullptr;
    QHBoxLayout *thumbLayout_ = nullptr;
    QLabel *hintLabel_ = nullptr;
    QPushButton *snapButton_ = nullptr;
    QPushButton *okButton_ = nullptr;
};

#endif // CAPTURE_DIALOG_H
