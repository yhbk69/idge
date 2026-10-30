#pragma once

// ============================================================================
// qr_decoder.h - 二维码解码（纯逻辑，不依赖 GUI / OpenCV）
// ============================================================================
//
// 从智能识别页里整块提取出来的 ZBar 解码。为什么要有这个模块：
// 登录页与智能识别页都要解同一套码，任何一份本地实现都是第二条分叉，
// 而扫码能力取决于**编译期**有没有 HAS_ZBAR、解不出来是什么层次的原因
// （没库 / 没码 / 图读不到）两处都必须一致地告诉用户。
//
// 本文件只用 Qt Core + Qt Gui（QImage），不带 OpenCV/RKNN 头，
// 因此可以脱离摄像头离线自测（tests/test_construction_qr.cpp）。
//
// 命中矩形用 QRect 而不是 cv::Rect：唯一消费者是结果框上画箭头定位中心点。
//
// 三个入口的关系：
//   scanQrFromRGBA  ← 解码器映射指针直给的帧（唯一真正做 ZBar 的地方）
//   scanQrFromImage ← QImage（含 RGB888/JPEG 解码后的任意格式，内部转 RGBA8888）
//   scanQrFromImageFile ← 落盘的码图，无摄像头环境下验证解码链
//
// ============================================================================

#include <QImage>
#include <QRect>
#include <QString>

#include <cstdint>
#include <vector>

namespace qrcode {

struct QrHit {
    QRect rect;       // 帧内像素坐标
    QString content;  // 码里的原文（UTF-8）
};

// 本次构建有没有编进 ZBar。false 时下面所有解码函数都返回 false。
bool decodeAvailable();

// 不可用时的中文原因（含补齐路径），两处调用点共用，别各写一份。
QString unavailableReason();

bool scanQrFromRGBA(const uint8_t *rgba, int width, int height, int stride,
                    std::vector<QrHit> *out, QString *err);

bool scanQrFromImage(const QImage &image, std::vector<QrHit> *out, QString *err);

bool scanQrFromImageFile(const QString &path, std::vector<QrHit> *out, QString *err);

} // namespace qrcode
