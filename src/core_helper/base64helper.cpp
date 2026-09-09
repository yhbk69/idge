#include "base64helper.h"
#include "qbuffer.h"
#include "qdebug.h"

// ============================================================================
// imageToBase64 - 图片转Base64字符串（返回QString）
// ============================================================================
QString Base64Helper::imageToBase64(const QImage &image)
{
    return QString(imageToBase64x(image));
}

// ============================================================================
// imageToBase64x - 图片转Base64字节数组（核心实现）
// 流程：QImage -> JPEG编码 -> Base64编码
// 注意：此操作可能比较耗时，建议在线程中执行
// ============================================================================
QByteArray Base64Helper::imageToBase64x(const QImage &image)
{
    QByteArray data;
    QBuffer buffer(&data);

    // 将QImage保存到内存缓冲区（JPEG格式）
    image.save(&buffer, "JPG");

    // 将JPEG二进制数据转换为Base64编码
    data = data.toBase64();
    return data;
}

// ============================================================================
// base64ToImage - Base64字符串转图片（返回QImage）
// ============================================================================
QImage Base64Helper::base64ToImage(const QString &data)
{
    return base64ToImagex(data.toUtf8());
}

// ============================================================================
// base64ToImagex - Base64字节数组转图片（核心实现）
// 流程：Base64解码 -> 二进制数据 -> QImage加载
// 注意：此操作可能比较耗时，建议在线程中执行
// ============================================================================
QImage Base64Helper::base64ToImagex(const QByteArray &data)
{
    QImage image;
    // 先Base64解码，再从二进制数据加载图片
    image.loadFromData(QByteArray::fromBase64(data));
    return image;
}

// ============================================================================
// textToBase64 - 文本字符串转Base64编码
// 使用UTF-8编码转换后进行Base64编码
// ============================================================================
QString Base64Helper::textToBase64(const QString &text)
{
    return QString(text.toUtf8().toBase64());
}

// ============================================================================
// base64ToText - Base64编码转文本字符串
// 先Base64解码，再使用UTF-8解码为原始文本
// ============================================================================
QString Base64Helper::base64ToText(const QString &text)
{
    return QString(QByteArray::fromBase64(text.toUtf8()));
}
