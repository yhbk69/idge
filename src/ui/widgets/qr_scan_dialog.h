#pragma once

// ============================================================================
// qr_scan_dialog.h - 摄像头扫码对话框（登录页与智能识别页共用）
// ============================================================================
//
// 为什么只能有一份：扫码取的是 config.json 的 smartRecognition.camera 设备节点
// （本板 /dev/video22），**同一设备节点不能被开两次**（ConfigManager.h 明写）。
// 登录页和智能识别页各写一套预览+取帧+解码，就等于两个人抢同一个摄像头。
//
// 三个入口按"调用方需要多少结果"分层，摄像头取址、PreviewOnly 解码、抓帧、
// 解码这四步在三个入口里是同一段代码：
//   captureFrame()    → 只拿到一张拍下来的帧（要自己处理画面的调用方）
//   scan()            → 只要码里的原文，取第一个命中，不弹结果框（登录页）
//   openFullScanner() → 取帧 + 解码 + 结果框：单码直显，多码在冻结图上点选（智能识别页）
//
// HAS_ZBAR 没编进来时三个入口都不静默返回：err 给出中文原因（qrcode::unavailableReason）。
//
// ============================================================================

#include <QImage>
#include <QString>

class QWidget;
class FFmpegVideoDecoder;

// 从 PreviewOnly 解码器取当前帧并转成 QImage(RGB888)。
// 二维码流程和 OCR 流程都要"拍一张"，取帧实现只有这一份（原先长在智能识别页里）。
QImage captureFrameAsImage(FFmpegVideoDecoder *decoder);

class QrScanDialog {
public:
    // 本次构建能不能扫码（转发 qrcode::decodeAvailable）
    static bool available();

    // 取一帧。true=拿到帧；false 时若 err 为空表示用户直接关掉了对话框（不算错误）。
    static bool captureFrame(QWidget *parent, QImage *frame, QString *err);

    // 扫码一步到位：取帧 + 解码，content 取第一个命中的原文。
    static bool scan(QWidget *parent, QString *content, QString *err);

    // 完整流程（含结果框），智能识别页的二维码卡片调这一个。
    static void openFullScanner(QWidget *parent);

private:
    QrScanDialog() = delete;
};
