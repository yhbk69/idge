#include "qr_decoder.h"

#include <QFileInfo>

#ifdef HAS_ZBAR
#include <zbar.h>

#include <algorithm>
#endif

namespace qrcode {

namespace {

const char kGrayFormat[] = "Y800";

} // namespace

bool decodeAvailable()
{
#ifdef HAS_ZBAR
    return true;
#else
    return false;
#endif
}

QString unavailableReason()
{
#ifdef HAS_ZBAR
    return QString();
#else
    // 不是"板子上没装 zbar"，而是这次 cmake 没找到 3rdparty/zbar：
    // 新克隆只带头文件和 zbar.pc，libzbar.so/.a 被 .gitignore 挡住，需照
    // 3rdparty/README.md 的流程补进 3rdparty/zbar/lib/ 后重新 configure。
    return QString::fromUtf8("本次构建未找到 ZBar（3rdparty/zbar/lib 缺库），"
                             "补齐后重新编译才能扫码。");
#endif
}

bool scanQrFromRGBA(const uint8_t *rgba, int width, int height, int stride,
                    std::vector<QrHit> *out, QString *err)
{
    if (!out)
        return false;
    out->clear();

#ifndef HAS_ZBAR
    Q_UNUSED(rgba);
    Q_UNUSED(width);
    Q_UNUSED(height);
    Q_UNUSED(stride);
    if (err) *err = unavailableReason();
    return false;
#else
    if (!rgba || width <= 0 || height <= 0 || stride < width * 4) {
        if (err) *err = QString::fromUtf8("帧数据无效，无法解码二维码");
        return false;
    }

    // RGBA → 灰度（ITU-R BT.601 系数），按调用方给的 stride 逐行取样，
    // 避免 QImage/OpenCV 各自的行对齐差异
    std::vector<uint8_t> gray(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; ++y) {
        const uint8_t *srcRow = rgba + static_cast<size_t>(y) * stride;
        uint8_t *dstRow = gray.data() + static_cast<size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const int r = srcRow[x * 4 + 0];
            const int g = srcRow[x * 4 + 1];
            const int b = srcRow[x * 4 + 2];
            dstRow[x] = static_cast<uint8_t>((r * 77 + g * 150 + b * 29) >> 8);
        }
    }

    zbar::ImageScanner scanner;
    scanner.set_config(zbar::ZBAR_QRCODE, zbar::ZBAR_CFG_ENABLE, 1);

    zbar::Image zbar_image(width, height, kGrayFormat, gray.data(),
                           static_cast<size_t>(width) * height);
    scanner.scan(zbar_image);

    for (zbar::Image::SymbolIterator symbol = zbar_image.symbol_begin();
         symbol != zbar_image.symbol_end(); ++symbol) {
        QrHit hit;
        const std::string rawData = symbol->get_data();
        hit.content = QString::fromUtf8(rawData.c_str(),
                                        static_cast<int>(rawData.size()));

        int minX = width, minY = height, maxX = 0, maxY = 0;
        const int locationSize = symbol->get_location_size();
        for (int i = 0; i < locationSize; ++i) {
            minX = std::min(minX, symbol->get_location_x(i));
            maxX = std::max(maxX, symbol->get_location_x(i));
            minY = std::min(minY, symbol->get_location_y(i));
            maxY = std::max(maxY, symbol->get_location_y(i));
        }
        // 没有定位点时（异常符号）退化成整帧，调用方只用它定中心点，不至于算出负尺寸
        if (locationSize <= 0) {
            minX = 0;
            minY = 0;
            maxX = width;
            maxY = height;
        }
        hit.rect = QRect(minX, minY, std::max(0, maxX - minX), std::max(0, maxY - minY));
        out->push_back(hit);
    }
    return true;
#endif // HAS_ZBAR
}

bool scanQrFromImage(const QImage &image, std::vector<QrHit> *out, QString *err)
{
    if (image.isNull()) {
        if (err) *err = QString::fromUtf8("图像为空，无法解码二维码");
        return false;
    }
    if (!decodeAvailable()) {
        if (err) *err = unavailableReason();
        return false;
    }
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    return scanQrFromRGBA(rgba.constBits(), rgba.width(), rgba.height(),
                          rgba.bytesPerLine(), out, err);
}

bool scanQrFromImageFile(const QString &path, std::vector<QrHit> *out, QString *err)
{
    QImage image(path);
    if (image.isNull()) {
        // 只报路径与存在性，不把文件内容往日志里塞
        if (err) {
            *err = QString::fromUtf8("图片读取失败：%1（文件%2）")
                       .arg(path, QFileInfo(path).exists()
                                    ? QString::fromUtf8("存在但格式不支持")
                                    : QString::fromUtf8("不存在"));
        }
        return false;
    }
    return scanQrFromImage(image, out, err);
}

} // namespace qrcode
