#pragma once

// ============================================================================
// qr_scan_dialog.h - 摄像头扫码对话框（登录页与智能识别页共用）
// ============================================================================
//
// 为什么只能有一份：扫码取的是 config.json 的 smartRecognition.camera 设备节点
// （本板 /dev/video22），**同一设备节点不能被开两次**（ConfigManager.h 明写）。
// 登录页和智能识别页各写一套预览+解码，就等于两个人抢同一个摄像头。
//
// 交互口径（2026-10-30 改回实时扫描）：**打开即扫描**，没有快门键。解码器每出一帧
// 就在解码线程上按限流跑一次 ZBar；解出码即冻结画面、在窗格下方显示码内内容，
// 用户点「确认」才关闭窗格并返回内容；点「重新扫描」继续扫，点「取消」直接关窗格。
// 确认这一步就是误码的兜底（不需要旧版的连续多帧一致性判定）。
//
// 两个入口按"调用方需要多少结果"分层，摄像头取址、PreviewOnly 解码、实时扫描、
// 冻结确认这四步在两个入口里是同一段代码：
//   scan()            → 只要码里的原文（登录页，确认键文案「确认登录」）
//   openFullScanner() → 同一套窗格，多码可在冻结图上点选（智能识别页）
//
// HAS_ZBAR 未定义时两个入口都不静默返回：err 给出中文原因（qrcode::unavailableReason），
// 且**连预览都不开**——开了也解不出码，白占摄像头。
//
// ============================================================================

#include <QImage>
#include <QString>

class QWidget;
class FFmpegVideoDecoder;

// 从 PreviewOnly 解码器取当前帧并转成 QImage(RGB888)。
// 扫码窗格冻结画面与 OCR 流程都要"拿一帧"，取帧实现只有这一份。
QImage captureFrameAsImage(FFmpegVideoDecoder *decoder);

class QrScanDialog {
public:
    // 本次构建能不能扫码（转发 qrcode::decodeAvailable）
    static bool available();

    // 实时扫码一步到位：解出 → 用户点确认 → content 给出该码原文。
    // false 且 err 为空 = 用户自己关掉/取消了窗格，不是故障。
    static bool scan(QWidget *parent, QString *content, QString *err);

    // 完整窗格（含多码点选），智能识别页的二维码卡片调这一个。
    static void openFullScanner(QWidget *parent);

private:
    QrScanDialog() = delete;
};
