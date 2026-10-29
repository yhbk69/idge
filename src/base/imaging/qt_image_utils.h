// 文件职责：Qt 图像安全读取工具。用 OpenCV 解码图片再转 QPixmap，规避板端 Qt JPEG
// 插件与 libjpeg 的 ABI 冲突；OpenCV 解码失败时回退调用 ImageMagick convert 命令。
#pragma once
#include <QPixmap>
#include <QPixmapCache>
#include <QImage>
#include <QString>
#include <QProcess>
#include <QFileInfo>
#include <QDir>
#include <QDebug>
#include <algorithm>
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

// 抓拍 sidecar 缩略图路径：与整幅抓拍图同目录、同名加 "_thumb"。
// 写侧（ffmpeg_video_decoder.cpp 的 SnapWriter）与读侧都经本函数，命名规则只有一份。
inline QString thumbnailSidecarPath(const QString& path) {
    const QFileInfo fi(path);
    return fi.dir().filePath(fi.completeBaseName() + QStringLiteral("_thumb.jpg"));
}

// 加载等比缩略图（保持宽高比，装进 w×h 框内）。
//
// 【为什么单独做这个】报警面板一次要显示 20 张 64×48 缩略图，原先用 QPixmap(path)
// 整幅解码 1280×720 JPEG 再 SmoothTransformation 缩放，一次刷新实测 600~765ms 全花在
// GUI 线程上，足以让 4 路视频每次丢 8~10 帧。
//
// 板端实测（Cortex-A76，300KB 1280×720 JPG）：IMREAD_COLOR 23.2ms，
// IMREAD_REDUCED_COLOR_8 11.1ms（空闲时）/ 13~22ms（写盘线程同时往 eMMC 灌截图时）。
// 降采样只省掉 IDCT 与上采样，Huffman 仍要解完全部 MCU，所以这里省的是约一半，
// 不是一个数量级——因此优先读抓拍落盘时顺带写好的 sidecar（见 thumbnailSidecarPath），
// 整幅解码只作为老数据/写失败时的回退。
//
// 走 OpenCV 而不是 Qt 图像插件，与本文件头记录的 ABI 冲突约定一致。
// QPixmapCache 按 "路径@宽x高" 缓存，只在同一条报警的条目被删掉又重建时命中
// （面板走增量刷新，已有条目不会重建，所以稳态下基本全是 miss）。
//
// 返回空 QPixmap 表示图片读不出来，调用方自行显示占位文案。
inline QPixmap loadThumbnailScaled(const QString& path, int w, int h) {
    const QString key = path + QLatin1Char('@') +
                        QString::number(w) + QLatin1Char('x') + QString::number(h);
    QPixmap cached;
    if (QPixmapCache::find(key, &cached)) return cached;

    // 先判存在再解：本功能上线之前落盘的老抓拍没有 sidecar，直接 imread 会让
    // OpenCV 为每张图往 stderr 打一行 "can't open/read file" 警告（实测冷启动
    // 面板 20 条老报警就是 20 行噪声）。
    cv::Mat bgr;
    const QString sidecar = thumbnailSidecarPath(path);
    if (QFileInfo::exists(sidecar))
        bgr = cv::imread(sidecar.toStdString(), cv::IMREAD_COLOR);
    if (bgr.empty()) {
        bgr = cv::imread(path.toStdString(), cv::IMREAD_REDUCED_COLOR_8);
        // 源图不足目标尺寸 8 倍时，1/8 结果比框还小，放大出来是糊的 → 退回整幅解码
        if (bgr.empty() || bgr.cols < w || bgr.rows < h)
            bgr = cv::imread(path.toStdString(), cv::IMREAD_COLOR);
        if (bgr.empty()) return QPixmap();
    }

    const double ratio = std::min(double(w) / bgr.cols, double(h) / bgr.rows);
    // sidecar 本身就是按这个框出的，ratio==1 → 跳过 resize（否则白拷一次）
    if (ratio < 1.0)
        cv::resize(bgr, bgr, cv::Size(int(bgr.cols * ratio), int(bgr.rows * ratio)),
                   0, 0, cv::INTER_AREA);
    cv::cvtColor(bgr, bgr, cv::COLOR_BGR2RGB);
    QImage image(bgr.data, bgr.cols, bgr.rows, static_cast<int>(bgr.step), QImage::Format_RGB888);
    QPixmap thumb = QPixmap::fromImage(image.copy());
    QPixmapCache::insert(key, thumb);
    return thumb;
}
