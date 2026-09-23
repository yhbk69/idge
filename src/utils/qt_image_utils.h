#pragma once
#include <QPixmap>
#include <QImage>
#include <QString>
#include <QProcess>
#include <QFileInfo>
#include <QDebug>
#include <opencv2/opencv.hpp>

// Decode images with OpenCV to avoid Qt JPEG plugin/libjpeg ABI conflicts.
inline QPixmap loadPixmapSafe(const QString& path) {
    qInfo() << "[image] loadPixmapSafe path=" << path
            << "exists=" << QFileInfo(path).exists() << "size=" << QFileInfo(path).size();
    cv::Mat bgr = cv::imread(path.toStdString(), cv::IMREAD_COLOR);
    qInfo() << "[image] cv::imread empty=" << bgr.empty();
    if (bgr.empty()) {
        // Some target images use a JPEG codec incompatible with the Qt/OpenCV
        // runtime. ImageMagick provides a reliable fallback conversion path.
        QProcess convert;
        qInfo() << "[image] trying ImageMagick convert for" << path;
        convert.start(QStringLiteral("convert"), {path, QStringLiteral("png:-")});
        if (convert.waitForFinished(5000) && convert.exitCode() == 0) {
            const QImage converted = QImage::fromData(convert.readAllStandardOutput(), "PNG");
            if (!converted.isNull()) return QPixmap::fromImage(converted);
        }
        return QPixmap();
    }
    cv::cvtColor(bgr, bgr, cv::COLOR_BGR2RGB);
    QImage image(bgr.data, bgr.cols, bgr.rows, static_cast<int>(bgr.step), QImage::Format_RGB888);
    return QPixmap::fromImage(image.copy());
}
