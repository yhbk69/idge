#ifndef BASE64HELPER_H
#define BASE64HELPER_H

// ============================================================================
// Base64编码转换辅助类头文件
// 提供图片与Base64字符串的相互转换功能
// 作者: feiyangqingyun(QQ:517216493) 2016-12-16
//
// 主要功能：
// 1. 图片转Base64字符串（用于网络传输、数据存储）
// 2. Base64字符串转图片
// 3. 文本字符串与Base64互转
// 4. Qt6对Base64编码转换进行了重写，效率提升至少200%
// ============================================================================

#include <QImage>

#ifdef quc
class Q_DECL_EXPORT Base64Helper
#else
class Base64Helper
#endif

{
public:
    /**
     * @brief 图片转Base64字符串
     * @param image [in] QImage图片对象
     * @return QString Base64编码字符串
     */
    static QString imageToBase64(const QImage &image);

    /**
     * @brief 图片转Base64字节数组
     * @param image [in] QImage图片对象
     * @return QByteArray Base64编码字节数组
     */
    static QByteArray imageToBase64x(const QImage &image);

    /**
     * @brief Base64字符串转图片
     * @param data [in] Base64编码字符串
     * @return QImage 解码后的图片对象
     */
    static QImage base64ToImage(const QString &data);

    /**
     * @brief Base64字节数组转图片
     * @param data [in] Base64编码字节数组
     * @return QImage 解码后的图片对象
     */
    static QImage base64ToImagex(const QByteArray &data);

    /**
     * @brief 文本转Base64字符串
     * @param text [in] 原始文本
     * @return QString Base64编码字符串
     */
    static QString textToBase64(const QString &text);

    /**
     * @brief Base64字符串转文本
     * @param text [in] Base64编码字符串
     * @return QString 解码后的原始文本
     */
    static QString base64ToText(const QString &text);
};

#endif // BASE64HELPER_H
