#pragma once

// ============================================================================
// qr_scan_dialog.h - 摄像头扫码对话框（登录页与智能识别页共用）
// ============================================================================
//
// 为什么只能有一份：扫码取的是 config.json 的 smartRecognition.camera 设备节点
// （本板 /dev/video22），**同一设备节点不能被开两次**（ConfigManager.h 明写）。
// 登录页和智能识别页各写一套预览+解码，就等于两个人抢同一个摄像头。
//
// 交互口径：**打开即扫描**，没有快门键。解码器每出一帧就在解码线程上按限流跑一次 ZBar。
// 解出码之后怎么收尾，两个入口是**两档**（2026-11-04 定）：
//   scan()            → 登录页：**解出即返回，直接登录**。不冻结画面、不显示码内内容、
//                       没有确认键；多码同帧取第一颗（日志记一行）。码是否合法由
//                       ConstructionSession::startLogin 判，窗格不重复判。
//   openFullScanner() → 智能识别页：冻结画面 + 窗格下方显示码内内容 + 点「确认」才收，
//                       多码可在冻结图上点选。那是给人"看码内容"的入口，不是登录。
//
// 摄像头取址、PreviewOnly 解码、实时扫描、收尾这几步在两个入口里是同一段代码。
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

    // 登录页专用：解出即返回（不显示码内内容），content 给出该码原文。
    // false 且 err 为空 = 用户自己关掉/取消了窗格，不是故障。
    static bool scan(QWidget *parent, QString *content, QString *err);

    // 完整窗格（含多码点选），智能识别页的二维码卡片调这一个。
    static void openFullScanner(QWidget *parent);

private:
    QrScanDialog() = delete;
};
