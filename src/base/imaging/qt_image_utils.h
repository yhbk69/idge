/**
 * @file qt_image_utils.h
 * @brief base/imaging 域 —— Qt 侧图片安全解码（header-only，全部 inline）
 *
 * 所属模块：src/base/imaging。核心职责：让 UI/业务拿到 QPixmap 时**绕开板端 Qt
 * 图像插件**，解码统一走 OpenCV，失败再兜底 ImageMagick；顺带管抓拍缩略图的
 * sidecar 命名与"按目标框等比缩放"的读取。
 *
 * 为什么要有这个文件（而不是直接 QPixmap(path)）：板端 Qt 的 JPEG 插件与链进来的
 * libjpeg 存在 ABI 冲突，整幅解码现场照片会直接把进程崩掉；这不是"偶发解码失败"
 * 而是崩溃，所以 UI 侧展示照片一律走 loadPixmapSafe/loadThumbnailScaled，
 * 保存则走 QImage::save —— 不要绕开这几个封装再调 cv::imread/QPixmap(path)
 * （口径见 imaging/README.md 注意事项第 4 条）。
 *
 * 无状态、无锁：QPixmapCache 与 QProcess 各自线程安全，但 QPixmap 本身必须在
 * GUI 线程构造，所以这三个函数只应在界面线程调用。
 */
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

/**
 * @brief 整幅安全解码一张图片 → QPixmap
 * @param path 图片路径（本地文件；空串/不存在不报错，直接落到失败返回）
 * @return 解码后的 QPixmap；**两条兜底都失败时返回空 QPixmap**（isNull()==true），
 *         调用方需自备占位图。不抛异常。
 * @note 同步阻塞：cv::imread 整幅解码 + 失败时最多等 5000ms 的 QProcess。
 *       5000ms 是给外部 `convert` 进程留的余量，也是"UI 卡住 5 秒"的上限——
 *       列表/批量场景请改用 loadThumbnailScaled()，别在这里循环解整幅图。
 * @note 兜底依赖板子上真的装了 ImageMagick 的 `convert`；没装时 waitForFinished
 *       直接返回 false，表现与"图片坏了"完全一样，只能靠前一行 qInfo 区分。
 *       排查"照片显示不出来"时先 `which convert`。
 */
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
    // QImage 这里只是**引用** bgr.data（零拷贝构造），而 bgr 是本函数局部变量、
    // return 即析构，所以末尾的 image.copy() 不是优化而是必需——去掉它就是悬垂指针。
    // 传 bgr.step 而不是假定 w*3：IMREAD_COLOR 出来本来就是连续的 8UC3，但按 step
    // 写才对得上 loadThumbnailScaled 的同一套形状，将来换解码 flags 不会埋雷。
    QImage image(bgr.data, bgr.cols, bgr.rows, static_cast<int>(bgr.step), QImage::Format_RGB888);
    return QPixmap::fromImage(image.copy());
}

// 抓拍 sidecar 缩略图路径：与整幅抓拍图同目录、同名加 "_thumb"。
// 写侧（ffmpeg_video_decoder.cpp 的 SnapWriter）与读侧都经本函数，命名规则只有一份。
// ⚠ 改这里的后缀等于同时改读写两侧：老数据是按 _thumb.jpg 落的盘，
//   改名后读侧找不到 sidecar 会静默退成整幅解码（变慢但不报错），很难归因。
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
/**
 * @param path 原抓拍图路径（sidecar 由 thumbnailSidecarPath(path) 推导，不要传 sidecar 本身）
 * @param w/@h 目标框的宽高（像素）
 * @return 等比缩放后装进 w×h 框内的 QPixmap；读不到图返回空 QPixmap
 * @note ⚠ w 或 h 传 0 会让 ratio 算成 0，进而走到 cv::resize(dsize=0×0, fx=fy=0)，
 *       OpenCV 在那儿是断言失败直接 abort 整个进程——本函数**不做**这层防御，
 *       调用方（报警面板）传的必须是布局里量出来的正数尺寸。
 */
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
