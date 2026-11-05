// ============================================================================
// ConfigManager.cpp - 配置管理器实现
// ============================================================================
//
// 作用：
//   管理应用程序的配置信息，从 config.json 读取和保存配置。
//
// 配置文件结构 (config.json, schema V2)：
//   {
//     "video": {
//       "models": {                        // 模型参数表：键 = 模型库目录名
//         "yolo11n-ppe":  {"alarm_class": "un_helmet", "note": "PPE安全装备检测"},
//         "yolo11n-coco": {"alarm_class": "",          "note": "COCO通用检测"}
//       },
//       "channels": [                      // 下标即通道号（0-based），条目内 channel 字段仅供人看
//         {"channel": 0, "source": "assets/test/192.mp4", "note": "东门",
//          "enabled": true, "models": ["yolo11n-ppe", "yolo11n-coco"]}
//       ]
//     },
//     "detect":   {"conf_threshold":0.25, "nms_threshold":0.45, "class_num":80, "threads":3},
//                  // ⚠ 四个键当前只有 conf/nms 被界面读写，且都没接进推理（README 已知问题）；
//                  //    class_num/threads 无任何消费者，已删掉 getter：类别数属于每个模型，
//                  //    推理线程数 = 各通道模型数之和（一个模型一个 PpeTask 线程）。
//     "geofence": {"enabled":true, "mode":"inside_alarm", "alarmClass":"person",
//                  "throttleSeconds":10, "channels":{ "0":{...形状...} }},
//     "recording":{...}, "alarm_uplink":{...}, "pad_server":{...}, "database":{...}, "websocket":{...}, "gb28181":{...}
//   }
//
// 两条设计口径，改配置或加键前先对齐：
//   1) 配置里**没有模型路径**。model/library/<id>/{model.rknn,labels.txt} 是唯一事实源，
//      配置只说"哪一路用哪个库模型"，因此仓库搬迁/换库/改目录结构都不需要重配 JSON。
//   2) 报警类别是"模型的参数"（video.models[].alarm_class），不是通道或槽位的参数；
//      围栏报警例外——它不属于级联，只看 geofence.alarmClass（固定一类目标）。
//
// 老的 V1 结构（全局 5 槽 cascade.models + model.path + alarm.classes）由
// migrateSchemaV2() 在启动时一次性改写，见该函数注释。
//
// ============================================================================

#include "ConfigManager.h"
#include "runtime_paths.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QDir>
#include <QTextStream>

#include <algorithm>
// 摄像头节点实探测（V4L2）
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

namespace {

/**
 * @brief 探测可用的摄像头 /dev/videoN 节点
 *
 * 为什么要探测而不是写死：节点号由内核按探测顺序分配，换 USB 口、插拔、
 * 换 ISP/CSI 接线或换内核后都会漂移（板端历史上曾稳定落在 /dev/video41，
 * 换环境即失效），写死的路径正是"打不开摄像头"的直接原因。
 *
 * 两档结果（按优先级）：
 *   - 首选 MJPEG：USB UVC 摄像头的常态，CameraPreviewDecoder 可直接零重编码
 *     落盘原始码流抓拍；
 *   - 次选 NV12/YUYV/UYVY：RK3588 的 CSI 摄像头（ov13855 之类）经 rkisp
 *     mainpath 出来就是这两种裸格式，且**不提供 MJPEG**，只认 MJPEG 会把
 *     物理摄像头整个漏掉；rkcif 那些只出 10-bit Bayer 的节点则被排除，
 *     因为选中也会在 avformat_open_input 处失败。
 * @return 找到 MJPEG 节点立即返回；否则返回第一个裸格式采集节点；全无返回空串
 */
QString probeCameraNode()
{
    QDir dev(QStringLiteral("/dev"));
    const QStringList names = dev.entryList(QStringList(QStringLiteral("video*")), QDir::System);

    // 按尾号数值升序（字典序会让 video10 排在 video2 前，选中"最远"的节点）
    auto nodeNumber = [](const QString &name) {
        int i = name.size();
        while (i > 0 && name[i - 1].isDigit()) --i;
        return i == name.size() ? -1 : name.mid(i).toInt();
    };
    QStringList sorted = names;
    std::sort(sorted.begin(), sorted.end(), [&nodeNumber](const QString &a, const QString &b) {
        return nodeNumber(a) < nodeNumber(b);
    });

    QString raw_format_node;  // 次选：只出裸格式的节点（CSI 走 rkisp 的摄像头）
    for (const QString &name : sorted) {
        const QString path = dev.absoluteFilePath(name);
        const int fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK);
        if (fd < 0) continue;

        v4l2_capability cap{};
        const bool can_capture = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
                                 (cap.capabilities & (V4L2_CAP_VIDEO_CAPTURE |
                                                      V4L2_CAP_VIDEO_CAPTURE_MPLANE));
        bool serves_mjpeg = false;
        bool serves_raw = false;
        if (can_capture) {
            v4l2_fmtdesc fmt{};
            fmt.type = (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
                           ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
                           : V4L2_BUF_TYPE_VIDEO_CAPTURE;
            while (ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
                if (fmt.pixelformat == V4L2_PIX_FMT_MJPEG) { serves_mjpeg = true; break; }
                if (fmt.pixelformat == V4L2_PIX_FMT_NV12 ||
                    fmt.pixelformat == V4L2_PIX_FMT_YUYV ||
                    fmt.pixelformat == V4L2_PIX_FMT_UYVY) {
                    serves_raw = true;
                }
                ++fmt.index;
            }
        }
        ::close(fd);
        if (serves_mjpeg) return path;
        if (serves_raw && raw_format_node.isEmpty()) raw_format_node = path;
    }
    return raw_format_node;
}

// 智能识别/扫码摄像头的默认采集尺寸。"帧率优先"那一档：摄像头每帧的 CPU 颜色转换量、
// 喂给 ZBar 的像素量、OCR 抓下来的那张静图，三处都按这个数走。
static const char *const kDefaultCameraSize = "1280x720";

} // namespace

// ============================================================================
// 获取单例实例
// ============================================================================
// 使用静态局部变量保证线程安全的懒汉式单例
// C++11 标准保证静态局部变量的初始化是线程安全的
// ============================================================================
ConfigManager &ConfigManager::instance()
{
    static ConfigManager mgr;
    return mgr;
}

// ============================================================================
// 从 JSON 文件加载配置
// ============================================================================
// 如果文件不存在，自动创建默认配置并保存。
// 默认配置（schema V2）：
//   - 模型参数表：ppe 报 un_helmet、coco 不报警（都按模型库目录名索引，配置里没有路径）
//   - 4 路视频都指向 "assets/test/192.mp4"，每路挂满参数表里的两个模型
//   - 围栏：启用、inside_alarm、类别 person、节流 10 秒
//   - 置信度阈值 0.25 / NMS 0.45
//   - recording.enabled=true（报警必带关联片段）、alarm_uplink.enabled=false（默认不往外发）
//
// 参数：
//   - path: 配置文件路径（默认 data/config.json）
//
// ============================================================================
static QJsonObject makeRecordingDefaults();
static QJsonObject makeAlarmUplinkDefaults();
static QJsonObject makePadServerDefaults();
// 流注册表的 JSON 互转（定义在"视频流注册表"一节，makeVideoDefaults 要先用一次）
static QJsonObject sourceToJson(const VideoSourceCfg &s);

// 新装现场的默认 video 段。直接按 V2 写，是为了让全新配置根本不进迁移分支
// （迁移分支只在读到老文件时才该跑一次）。
static QJsonObject makeVideoDefaults()
{
    // 一条自带的测试片作为注册表样板：全新安装的通道就写成"引用"，
    // 让现场第一眼看到的结构就是"流在 sources 里、通道只写 source_id"。
    // 解析出来的地址仍是 assets/test/192.mp4，所以这批改默认值不改变任何行为。
    VideoSourceCfg demo;
    demo.id = QStringLiteral("file-demo");
    demo.type = QStringLiteral("file");
    demo.url = QStringLiteral("assets/test/192.mp4");
    demo.note = QStringLiteral("仓库自带测试片（改这几路就改这一条）");
    demo.loop = true;
    demo.hardwareDecode = true;

    QJsonArray sources;
    sources.append(sourceToJson(demo));

    const QJsonArray kModels{"yolo11n-ppe", "yolo11n-coco"};
    QJsonArray channels;
    for (int ch = 0; ch < 4; ++ch) {
        channels.append(QJsonObject{{"channel", ch},
                                    {"source_id", demo.id},
                                    {"source", ""},
                                    {"note", ""},
                                    {"enabled", true},
                                    {"models", kModels}});
    }
    return QJsonObject{
        {"models", QJsonObject{
            {"yolo11n-ppe", QJsonObject{{"alarm_class", "un_helmet"},
                                        {"note", "PPE安全装备检测"}}},
            {"yolo11n-coco", QJsonObject{{"alarm_class", ""},
                                         {"note", "COCO通用检测"}}},
        }},
        {"sources", sources},
        {"channels", channels},
    };
}

/**
 * @brief 载入整份 config.json 到 root_，并按需跑一次性迁移
 * @param path 路径；相对路径按**当前工作目录**解析（RuntimePaths::configFile() 就是相对
 *             路径，所以经 run.sh 起时会读到构建目录那份，见 runtime_paths.h 文件头）
 * @note 全程持 mutex_，因此本函数内的所有迁移分支**不得**回调任何会回头取本类锁的对象
 *       （ModelRegistry::resolveChannelSlots()/referenceCount() 首当其冲）——
 *       std::mutex 不可重入，那就是启动死锁。ConfigManager.h 顶部第 2 条是同一条教训。
 * @note ⚠ 三条失败路径的结果**并不都是"空配置"**，别按"load 失败就是默认值"理解：
 *       - 文件不存在：写入整套 schema V2 默认并 saveUnsafe() 落盘后**直接 return**
 *         （连 updateCache() 都不走，缓存值仍是构造期的 0.25/0.45/true/30——
 *          恰好与刚写进去的默认值相同，所以现状无害，但改默认值时这几格要一起改）；
 *       - JSON 语法错/文件损坏：root_ 被置空 → 全部 getter 走内置兜底，
 *         原文件内容被静默丢弃（下一次 save() 会把兜底结果覆盖回去）；
 *       - 打不开（权限/只读挂载）：只 qWarning，**root_ 保持上一次 load 的内容不变**——
 *         进程内重复调用 load() 时这一条会让人误以为"配置回滚了"。
 * @postcondition root_ 至多多出被补齐的那几段；迁移有改动就立即落盘（幂等：
 *                第二次启动 hasSchemaV2() 为真，整段迁移被跳过）。
 */
void ConfigManager::load(const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    configPath_ = path;
    QFile file(path);

    // 文件不存在时，创建默认配置
    if (!file.exists()) {
        // 视频配置（schema V2）：模型参数表 + 四路各自有序模型列表
        root_["video"] = makeVideoDefaults();

        // 检测参数配置
        // 只写有 getter 的键：class_num/threads 曾经也在这里，但推理侧从不读它们，
        // 出厂配置里留着一个能改、改了不生效的键就是假开关，故不再写入。
        root_["detect"] = QJsonObject{
            {"conf_threshold", 0.25},  // 置信度阈值
            {"nms_threshold", 0.45}   // NMS 阈值
        };

        // 报警视频录制（硬件编码环形缓冲区）
        root_["recording"] = makeRecordingDefaults();

        // 报警上行（P0-1：报警信息 + 截图 + 片段经 WS 推送）
        root_["alarm_uplink"] = makeAlarmUplinkDefaults();

        // Pad 接口 v1 的服务端（本机是服务端，Pad 连进来）。同样出厂关：开一个明文零鉴权
        // 的监听口必须人来决定。
        root_["pad_server"] = makePadServerDefaults();

        // 智能识别/扫码：用哪个节点 + 采多大分辨率。两个键都有 getter，所以写出来；
        // 此前这段根本不进默认配置，"用哪只摄像头、多少像素"全靠运行时猜、现场无法改。
        // camera 留空 = 走 probeCameraNode() 实探测；camera_size 是帧率与清晰度的旋钮。
        root_["smartRecognition"] = QJsonObject{
            {"camera", QString()},
            {"camera_size", QString::fromLatin1(kDefaultCameraSize)},
        };

        saveUnsafe();  // 保存默认配置
        return;
    }

    // 读取并解析 JSON 文件
    // 解析失败/文件损坏时不抛异常，而是把 root_ 置为空对象：
    // 此后所有 getter 会因取不到键而回退到内置默认值（0.25/0.45/"inside_alarm"/"person"...），
    // 保证程序仍可带默认配置启动；但注意这会静默丢弃用户原有配置。
    if (file.open(QIODevice::ReadOnly)) {
        QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        if (doc.isNull()) {
            qWarning() << "[ConfigManager] JSON 解析失败:" << path;
            root_ = QJsonObject();
        } else {
            root_ = doc.object();
        }
        file.close();
    } else {
        qWarning() << "[ConfigManager] 文件打开失败:" << path;
    }

    // ===== 一次性迁移（仅旧 schema 才走）=====
    // 顺序有讲究：migrateModelPaths 先把绝对路径/平铺路径归一到 model/library/<id>/model.rknn，
    // migrateSchemaV2 才能用纯字符串把库目录名提出来当模型键。
    // V2 文件里已经没有 model/cascade 两段，再跑路径迁移只会凭空把旧键写回去，所以一起挡掉。
    if (!hasSchemaV2(root_)) {
        bool changed = false;
        if (migrateModelPaths()) {
            qInfo() << "[ConfigManager] 已将旧模型路径归一到模型库规范 (model/library/)";
            changed = true;
        }
        if (migrateSchemaV2()) {
            qInfo() << "[ConfigManager] 已迁移到 schema V2：模型按通道配、报警类归到模型参数 (video.models/channels)";
            changed = true;
        }
        if (changed)
            saveUnsafe();
    }

    // 缺 recording 段时补齐默认值（enabled=true，报警片段是需求验收项），让开关在文件里可见
    if (migrateRecordingDefaults()) {
        qInfo() << "[ConfigManager] 已补齐报警视频录制默认配置 (recording/)";
        saveUnsafe();
    }

    // 缺 alarm_uplink 段时补齐默认值（enabled=false，默认不往外发；见 migrate 注释）
    if (migrateAlarmUplinkDefaults()) {
        qInfo() << "[ConfigManager] 已补齐报警上行默认配置 (alarm_uplink/)";
        saveUnsafe();
    }

    // 缺 pad_server 段时补齐默认值（enabled=true：这台设备的用途就是被 Pad 连）
    if (migratePadServerDefaults()) {
        qInfo() << "[ConfigManager] 已补齐 Pad 服务端默认配置 (pad_server/)";
        saveUnsafe();
    }

    // 缺 pad_http 段时补齐默认值。⚠ 默认 enabled=true，所以这一句补齐本身就意味着
    // 老配置在本次启动之后开始监听 8899（写面）——与 pad_server 那句"出厂默认开"同一个决定。
    if (migratePadHttpDefaults()) {
        qInfo() << "[ConfigManager] 已补齐 Pad HTTP 面默认配置 (pad_http/)，默认监听 8899";
        saveUnsafe();
    }

    // 缺 video.sources 段时补一个空数组（不改各通道已有的内联 source）
    if (migrateVideoSourcesDefaults()) {
        qInfo() << "[ConfigManager] 已补齐视频流注册表空段 (video.sources)";
        saveUnsafe();
    }

    // 从 root_ 更新缓存值（避免每次 getter 都反序列化 JSON）
    updateCache();
}

// ============================================================================
// 模型路径一次性迁移（load 内持锁调用，仅 schema V1 文件）
// ============================================================================
// 只做一件事：把旧写法归一成 model/library/<id>/model.rknn，好让紧跟其后的
// migrateSchemaV2() 用纯字符串提出库目录名当模型键。规则（详见
// plan/model_management.md §一"路径迁移规则"）：
//   1) 绝对路径去前缀：/任意前缀/model/xxx → model/xxx，仓库整体搬迁后配置不失效
//   2) 旧平铺模型路径 → 模型库路径（仅当库内文件确实存在时才改写）
// V2 文件里没有 model/cascade 两段，本函数不再被调用（配置里根本不存路径）。
// 返回是否发生了修改；调用方据此决定是否 saveUnsafe() 落盘。
// ============================================================================
static QString migrateOneModelPath(QString p)
{
    p = p.trimmed();
    if (p.isEmpty())
        return p;

    const int cut = p.indexOf("/model/");
    if (cut >= 0 && p.startsWith('/'))
        p = p.mid(cut + 1);  // "/xxx/model/yyy" → "model/yyy"

    static const struct { const char *legacySuffix; const char *libPath; } kMap[] = {
        { "yolo11n.rknn", "model/library/yolo11n-coco/model.rknn" },
        { "yolo11s.rknn", "model/library/yolo11s-coco/model.rknn" },
        { "yolo11m.rknn", "model/library/yolo11m-coco/model.rknn" },
    };
    for (const auto &m : kMap) {
        if (p.endsWith(m.legacySuffix) && QFile::exists(m.libPath)) {
            p = m.libPath;
            break;
        }
    }
    return p;
}

/**
 * @brief 把 root_ 里 V1 的模型路径就地归一（只在 !hasSchemaV2() 时被 load() 调用）
 * @return true=root_ 里至少有一个路径串被改写过（调用方据此决定是否 saveUnsafe()）
 * @note 前置：调用方必须已持有 mutex_。
 * @note TODO: 本函数是"读改写整个 cascade 段"，所以**即使 root_ 里原本没有 cascade**
 *       也会被写回一个 {"models": []} 空对象（末尾那两行是无条件赋值的）。
 *       现网触发条件很窄：只有 JSON 解析失败使 root_ 变空、或 V1 之前那种怪文件，
 *       且 changed=false 时不落盘，所以只在"有人随后调 save()"时把空段写进文件。
 *       不改行为，先登记；要修就把末尾赋值改成"仅当原段存在时回写"。
 */
bool ConfigManager::migrateModelPaths()
{
    bool changed = false;
    auto fix = [&changed](const QString &v) {
        const QString n = migrateOneModelPath(v);
        if (n != v)
            changed = true;
        return n;
    };

    // 全局 model.path / model.label
    QJsonObject model = root_["model"].toObject();
    if (!model.isEmpty()) {
        if (model.contains("path")) model["path"] = fix(model["path"].toString());
        if (model.contains("label")) model["label"] = fix(model["label"].toString());
        root_["model"] = model;
    }

    // cascade.models[] 每槽 path / label
    QJsonObject cascade = root_["cascade"].toObject();
    QJsonArray arr = cascade["models"].toArray();
    for (int i = 0; i < arr.size(); ++i) {
        QJsonObject o = arr[i].toObject();
        const QString p = o["path"].toString();
        const QString l = o["label"].toString();
        const QString np = fix(p);
        const QString nl = fix(l);
        if (np != p) o["path"] = np;
        if (nl != l) o["label"] = nl;
        arr[i] = o;
    }
    cascade["models"] = arr;
    root_["cascade"] = cascade;

    return changed;
}

// ============================================================================
// schema V1 → V2 一次性迁移（load 内持锁调用）
// ============================================================================
// V1：全局一份 5 个级联槽（槽 1 的模型取 model.path、槽 2~5 取 cascade.models[i].path，
//     而报警类/标签/备注一律取 cascade.models[i]），四路视频共用同一套。
// V2：video.models  = 按"模型库目录名"索引的参数表（报警类别是模型的参数）
//     video.channels = 4 项数组，下标即通道号，各带自己的有序模型列表
//
// 迁移把"哪些模型在用"折成参数表条目、把"哪一路在用"写成通道列表。
// V1 本来就是四路共用一套，所以把同一份模型列表广播到每个通道，行为完全等价。
//
// ⚠ 本函数在 load() 持锁上下文里跑，绝不能回调 ModelRegistry：它的
//   referenceCount()/resolveChannelSlots() 会回头取 ConfigManager 的锁，
//   std::mutex 不可重入 → 启动死锁。所以路径→库 id 只做纯字符串换算。
// ============================================================================

// "…/model/library/<id>/model.rknn" → "<id>"；不是这个规范写法返回空串
// （平铺的 model/yolo11n.rknn、自定义目录都拿不到 id，只能让人在模型管理页导入后重配）
static QString pathToLibraryId(const QString &rawPath)
{
    const QString p = rawPath.trimmed();
    if (p.isEmpty())
        return QString();

    // 相对写法 "model/library/<id>/model.rknn" 前面没有 '/'，补一个再统一找标记，
    // 免得绝对路径与相对路径各写一套解析
    const QString marked = QLatin1Char('/') + QDir::fromNativeSeparators(p);
    static const QString kMark = QStringLiteral("/model/library/");
    const int at = marked.indexOf(kMark);
    if (at < 0)
        return QString();

    const QString tail = marked.mid(at + kMark.size());
    const int slash = tail.indexOf('/');
    if (slash <= 0)
        return QString();
    // 只认库规范命名：目录名就是 id，模型文件名固定 model.rknn
    if (tail.mid(slash + 1) != QLatin1String("model.rknn"))
        return QString();
    return tail.left(slash);
}

// 某个库模型的 labels.txt 里有没有这个类别名（迁移时用来挑围栏类别）
// 直接读文件而不走 ModelRegistry，原因同上（避免回头取锁）
static bool libraryHasClass(const QString &libId, const QString &cls)
{
    QFile f(QStringLiteral("model/library/%1/labels.txt").arg(libId));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        if (ts.readLine().trimmed() == cls)
            return true;
    }
    return false;
}

/**
 * @brief root_ 是否已是 V2 schema
 * @return 判据是 video.channels 这一个键存在（不是"键非空数组"）
 * @note 用作一次性闸门：migrateSchemaV2() 迁移完会把 V1 的 model/cascade/alarm/
 *       video.channelN/video.notes 全删掉，所以这里一旦为真就永久为真，
 *       第二次启动直接跳过整段迁移（可用 md5sum 对比两次启动后的文件验证幂等）。
 *       ⚠ 手工把 "channels": [] 删空但留着键，也会被这里判成"已迁移"——
 *       这个键是闸门而不是内容校验，别改成判空。
 */
bool ConfigManager::hasSchemaV2(const QJsonObject &root)
{
    return root["video"].toObject().contains(QStringLiteral("channels"));
}

bool ConfigManager::migrateSchemaV2()
{
    if (hasSchemaV2(root_))
        return false;

    QJsonObject video = root_["video"].toObject();
    const QJsonArray cascadeArr = root_["cascade"].toObject()["models"].toArray();
    const QString primaryPath = root_["model"].toObject()["path"].toString();

    // ---- 1. 非空槽 → 参数表条目（同库多槽合并）----
    QStringList orderedIds;  // 按槽序去重后的库 id，就是要广播给每个通道的模型列表
    QJsonObject params;
    for (int i = 0; i < cascadeArr.size(); ++i) {
        const QJsonObject slot = cascadeArr[i].toObject();
        const QString path = (i == 0 && !primaryPath.trimmed().isEmpty())
                                 ? primaryPath          // V1 有效语义：槽 1 取全局 model.path
                                 : slot["path"].toString();
        if (path.trimmed().isEmpty())
            continue;  // 空槽 = 不启用

        const QString libId = pathToLibraryId(path);
        if (libId.isEmpty()) {
            qWarning() << "[ConfigManager] 该模型不在库规范目录 (model/library/<id>/model.rknn) 内,"
                          " 迁移后无法按通道引用，请在『模型管理』页导入后再配:" << path;
            continue;
        }

        QJsonObject o = params[libId].toObject();
        const QString alarmClass = slot["alarm_class"].toString().trimmed();
        const QString existing = o["alarm_class"].toString();
        if (!alarmClass.isEmpty() && existing.isEmpty()) {
            o["alarm_class"] = alarmClass;
        } else if (!alarmClass.isEmpty() && !existing.isEmpty() && alarmClass != existing) {
            // 同一个库模型出现在多个槽位却配了不同报警类：V2 里它是模型属性，只能留一个。
            // 保留先出现的那个并点名，否则操作者会以为两个类别都在生效。
            qWarning() << "[ConfigManager] 模型" << libId
                       << "在多个槽位配了不同报警类，保留" << existing
                       << "，忽略" << alarmClass << "（V2 里报警类是模型参数，只能有一个）";
        }

        const QString label = slot["label"].toString().trimmed();
        const QString libLabel = QStringLiteral("model/library/%1/labels.txt").arg(libId);
        if (!label.isEmpty() && label != libLabel)
            o["label"] = label;  // 只有指向别处的标签才留覆盖，库内默认标签不写进配置

        const QString note = slot["note"].toString();
        if (o["note"].toString().isEmpty() && !note.isEmpty())
            o["note"] = note;

        if (!orderedIds.contains(libId))
            orderedIds.append(libId);
        params[libId] = o;
    }
    // 条目补齐：alarm_class/note 恒有键（空串也要写出来），现场才知道这是"配过的不报警"
    // 而不是"漏配"；label 只在覆盖时才出现。
    // ⚠ conf_threshold 反过来——**缺键本身就是"跟随全局 detect.conf_threshold"的载体**，
    //    所以这里绝不补齐：写一个 0.25 进去会让这个模型从此不再跟着全局档走。
    for (const QString &id : orderedIds) {
        QJsonObject o = params[id].toObject();
        if (!o.contains("alarm_class")) o["alarm_class"] = QString();
        if (!o.contains("note")) o["note"] = QString();
        params[id] = o;
    }

    // ---- 2. video.channel1..4 + notes[] → channels[]（四路广播同一份模型列表）----
    const QJsonArray notes = video["notes"].toArray();
    QJsonArray channels;
    for (int ch = 0; ch < channelCount(); ++ch) {  // channelCount() 是常量内联，持锁调用安全
        QJsonArray models;
        for (const QString &id : orderedIds) models.append(id);
        channels.append(QJsonObject{
            {"channel", ch},
            {"source", video[QStringLiteral("channel%1").arg(ch + 1)].toString()},
            {"note", ch < notes.size() ? notes[ch].toString() : QString()},
            {"enabled", true},
            {"models", models},
        });
    }

    video["models"] = params;
    video["channels"] = channels;
    video.remove(QStringLiteral("notes"));
    for (int ch = 1; ch <= channelCount(); ++ch)
        video.remove(QStringLiteral("channel%1").arg(ch));
    root_["video"] = video;

    // ---- 3. geofence.alarmClasses[] → alarmClass 单值 ----
    // V1 那个列表是"哪些类名允许进围栏判定"的白名单，V2 里围栏只认一类目标，
    // 所以取"本配置里真有模型能产出它"的第一项：现场 ["person","people"] 且挂了
    // COCO → person；只挂 PPE 的现场 → people。挑不到就回落 person（与旧硬编码一致）。
    QJsonObject gf = root_["geofence"].toObject();
    if (!gf.isEmpty()) {
        const QJsonArray list = gf["alarmClasses"].toArray();
        QString pick;      // 第一个"本配置的模型真能产出"的类名
        QString fallback;  // 第一个非空类名（没人能产出时用它，保持与旧配置同源）
        for (const auto &v : list) {
            const QString cls = v.toString().trimmed();
            if (cls.isEmpty())
                continue;
            if (fallback.isEmpty())
                fallback = cls;
            for (const QString &id : orderedIds) {
                if (libraryHasClass(id, cls)) { pick = cls; break; }
            }
            if (!pick.isEmpty())
                break;
        }
        if (pick.isEmpty())
            pick = fallback.isEmpty() ? QStringLiteral("person") : fallback;
        gf["alarmClass"] = pick;
        gf.remove(QStringLiteral("alarmClasses"));
        if (!gf.contains(QStringLiteral("throttleSeconds")))
            gf["throttleSeconds"] = 10;  // 围栏报警从此有自己的节流下限（V1 是每帧一条）
        root_["geofence"] = gf;
    }

    // ---- 4. 旧键整段删除：路径不再是配置项、全局报警类列表不再有消费者 ----
    root_.remove(QStringLiteral("cascade"));
    root_.remove(QStringLiteral("model"));
    root_.remove(QStringLiteral("alarm"));

    qInfo() << "[ConfigManager] 迁移结果 模型参数表:" << orderedIds;
    for (int ch = 0; ch < channels.size(); ++ch) {
        const QJsonObject o = channels[ch].toObject();
        qInfo() << "[ConfigManager]   通道" << ch << "源=" << o["source"].toString()
                << "模型=" << o["models"].toArray() << "围栏类别="
                << root_["geofence"].toObject()["alarmClass"].toString();
    }
    return true;
}

// ============================================================================
// 报警视频录制 / 报警上行 默认值一次性迁移（load 内持锁调用）
// ============================================================================
// 老配置里没有这些段时补上全默认值。
// 目的不是给程序兜底（getter 本来就有默认值），而是让这一段落进文件，
// 现场运维能直接看到并改开关，否则要凭空手写一段 JSON。
// 已有段一律不动：现场改过的值不能被启动流程覆盖。
// ============================================================================
static QJsonObject makeRecordingDefaults()
{
    return QJsonObject{
        {"enabled", true},           // 报警关联片段是需求验收项（requirements.md §三①），出厂即开
        {"codec", "hevc"},           // D2=b：主用 H.265（带宽减半）
        {"buffer_seconds", 8},       // 环形缓冲区 8 秒：报警窗口是"前3+后2"共 5 秒，
                                     //   但缓冲区按**包数**算（buffer_seconds × 流报帧率），
                                     //   入包节拍被 pacer 限到 ≤30fps，再加 gop_seconds=1 的
                                     //   关键帧对齐最坏吃掉前向 1 秒 —— 5 秒是零余量卡边，故给 8
        {"bitrate", 2000000},        // 2 Mbps CBR
        {"gop_seconds", 1}           // 1 秒一个 IDR，报警片段起点对齐误差 ≤1s
    };
}

bool ConfigManager::migrateRecordingDefaults()
{
    if (root_.contains("recording")) {
        return false;
    }
    root_["recording"] = makeRecordingDefaults();
    return true;
}

// enabled 默认 false 的理由和 recording 相反：这是**往外发数据**，
// 且当前链路是明文 ws + 零鉴权，
// 装完就自动往配置里的地址推报警，等于默认把数据送上网，必须人来打开。
static QJsonObject makeAlarmUplinkDefaults()
{
    return QJsonObject{
        {"enabled", false},          // 总开关：false 时 AlarmUplink 不订阅、不建线程、零分配
        {"send_snapshot", true},     // 第一段（报警信息）里带上截图的传输计划
        {"send_clip", true},         // 第二段带视频片段；recording.enabled=false 时自动降级为不发
        {"media_transport", "url"},  // 媒体怎么交出去："url" = 本机起 http 文件服务、JSON 只发地址
                                     //   （默认）；"ws_binary" = 字节走 WS 二进制分块帧（已实测，
                                     //   但会占 GUI 线程写缓冲，且要求对端能收大帧，故不作默认）
        {"media_http_port", 8090},   // media_transport="url" 时内嵌 http 服务的监听口。
                                     //   取 8090 而非 80：板端 uid 1000 + ip_unprivileged_port_start=1024
        {"media_base_url", ""},      // 地址前缀，空 = 自动（本机第一个非回环 IPv4 + 实际绑定端口）。
                                     //   多网卡猜错、或对端只能经 NAT/域名访问时在这里写死
        {"chunk_size", 262144},      // 二进制帧分块大小 256KB：Qt 出站按"一次调用=一个整帧"设计，
                                     //   对端 maxFrameSize 更小时调这里，不用改代码
        {"ack_timeout_ms", 15000},   // 等 uplink_ack 的超时（仅 expect_ack=true 时有意义）
        {"max_attempts", 3},         // 同一项最多重投几次，超过判 failed 并打 warning
        {"max_pending", 16},         // 待确认队列上限，满了丢最旧（与 ClipWriter/SnapWriter 同口径）
        {"expect_ack", false}        // 对端当前不回 uplink_ack；true 时必须等确认才算完成
    };
}

bool ConfigManager::migrateAlarmUplinkDefaults()
{
    if (root_.contains("alarm_uplink")) {
        return false;
    }
    root_["alarm_uplink"] = makeAlarmUplinkDefaults();
    return true;
}

// Pad 接口 v1 的服务端（plan/edge-pad-v1.md §2–§3）。enabled **默认 true**（2026-11-04 现场定：
// 这台设备的用途就是被 Pad 连，出厂就该能连），所以缺键的老配置也会直接开始监听。
// 代价说清楚：这是一个明文零鉴权的入站口，局域网里任何设备都能连进来收报警、拿证据地址；
// 唯一的门是握手时的 Origin 白名单（见 pad_ws_server.cpp 的 originAllowed），
// 不想要就显式写 `"enabled": false`。
static QJsonObject makePadServerDefaults()
{
    return QJsonObject{
        {"enabled", true},         // true = listen 并订阅 AlarmManager；false = 完全不占口
        {"port", 8900},            // A4 已定（2026-11-04）：**WS 走 8900，8899 留给 /api/ HTTP**。
                                   //   两只监听器本来就不能同口（QWebSocketServer 独占一只），
                                   //   对方给的分配把这件事解开了；改默认值不动 media_http_port(8090)
        {"ws_path", "/"},          // 只进日志：QWebSocketServer 不按路径筛连接，Pad 连哪条路径都收
        {"device_id", ""},         // hello_ack.deviceId（例 EDGE-05-007）；空 = 该键省略
        {"media_base_url", ""},    // 证据 URL 前缀；空 = 用内嵌 http 服务的自动地址，
                                   //   http 服务没起来就不发地址（宁缺勿假）
        {"sync_limit", 500}        // 一次 sync_result 最多带几条。v1 没给分页规则，
                                   //   先设上限；游标取本次返回里的最大 update_time，不会漏下一批
    };
}

bool ConfigManager::migratePadServerDefaults()
{
    if (root_.contains("pad_server")) {
        return false;
    }
    root_["pad_server"] = makePadServerDefaults();
    return true;
}

// Pad 接口 v1 的 HTTP `/api/` 面（plan/edge-pad-v1.md §4~§7、§9）。
// enabled 默认 **true** 与 pad_server 同一条判断：这台设备的用途就是被 Pad 连。
// 代价必须说清：这是**写**面——局域网内任何设备都能改报警状态（处置/误报/已读）
// 与围栏（建/改/下发/停用）。规约 §2.3 认了"默认不做 Token 鉴权"，本机照做，
// 但不想要就显式写 `"enabled": false`（关掉是不 bind，不是 bind 了再拒）。
static QJsonObject makePadHttpDefaults()
{
    return QJsonObject{
        {"enabled", true},                  // 总开关
        {"port", 8899},                     // A4：8899=/api/、8900=WS、8090=媒体文件
        {"max_body_mb", 26},               // §4.2 五张 5MB 照片 + JSON 余量
        {"expose_stream_credentials", false},  // §6.1 的 liveUrl 默认剥掉视频源口令
        {"fence_frames", true},             // §5.3 apply 要渲染合成画面 PNG
        {"dispose_options", QJsonArray()},  // 空 = §4.1 用代码内置那两条措辞
    };
}

bool ConfigManager::migratePadHttpDefaults()
{
    if (root_.contains("pad_http")) {
        return false;
    }
    root_["pad_http"] = makePadHttpDefaults();
    return true;
}

bool ConfigManager::migrateVideoSourcesDefaults()
{
    if (!root_.contains("video")) {
        return false;   // 连 video 段都没有（V1 之前的怪文件），不归这里管
    }
    QJsonObject video = root_["video"].toObject();
    if (video.contains("sources")) {
        return false;   // 已有段一律不动：现场改过的流不能被启动流程覆盖
    }
    // 只补一个**空数组**，不猜内容：现场的四路此刻还是内联 source，解析走兜底，
    // 补出来的空表既不改行为，也让运维看得见"这里能写流"。
    video["sources"] = QJsonArray();
    root_["video"] = video;
    return true;
}

// ============================================================================
// 从 root_ 更新缓存值
// ============================================================================
// 将高频访问的配置值缓存到成员变量，getter 直接返回缓存值
// 避免每次调用 getter 都做 root_["xxx"].toObject()["yyy"] 的临时对象创建
// ============================================================================
/**
 * @brief 把 4 个高频值从 root_ 刷进成员缓存
 * @note ⚠ 唯一调用点是 load() 的末尾。也就是说缓存只在"载入"这一刻与 root_ 对齐一次，
 *       此后 root_ 与 confThreshold_/nmsThreshold_/storeDetections_/
 *       detectionRetentionDays_ 的一致性**全靠那四个 setter 同时写两边**。
 *       绕过 setter 直接改 root_（例如新增迁移分支里写 detect.conf_threshold）
 *       会得到"JSON 里是新值、confThreshold() 还是旧值"的现象——
 *       这也是为什么只有这 4 个键有缓存、其余 getter 一律现读 root_。
 * @note 缺键时的兜底值写死在这里（0.25/0.45/true/30），与 ConfigManager.h 里那四个
 *       缓存成员的类内默认值必须同数（0.25/0.45/true/30）；否则"从没 load 过"与
 *       "load 了但缺键"两种情况会给出不同的数，而这两种在测试里都会出现。
 *       detect 段的出厂值另在 load() 的文件不存在分支里写一遍，改默认时三处一起看。
 */
void ConfigManager::updateCache()
{
    QJsonObject detect = root_["detect"].toObject();
    confThreshold_ = detect["conf_threshold"].toDouble(0.25);
    nmsThreshold_ = detect["nms_threshold"].toDouble(0.45);

    QJsonObject db = root_["database"].toObject();
    storeDetections_ = db["storeDetections"].toBool(true);
    detectionRetentionDays_ = db["detectionRetentionDays"].toInt(30);
}

// ============================================================================
// 将当前配置保存到 JSON 文件（内部不加锁版本）
// ============================================================================
/**
 * @brief 把 root_ 整体序列化写回 configPath_（**调用方必须已持有 mutex_**）
 * @note ⚠ 前置条件违反就是死锁：std::mutex 不可重入，公开接口（save/getter/setter）
 *       内部已加锁，在那些路径上再调本函数会二次加锁自锁。
 *       这也是 saveUnsafe() 不放进 public 的原因——见 ConfigManager.h 顶部第 2 条。
 * @note 非原子写：QIODevice::WriteOnly 是**截断覆写**，进程在 write 中途崩溃会留下
 *       半截 JSON。下一次 load() 解析失败会把 root_ 置空并全走内置兜底，
 *       等于用一次崩溃换掉整份现场配置（本仓没有备份/原子改名机制，已知取舍）。
 * @note 失败静默：打开文件失败既不返回错误也不打日志（与 load() 有 qWarning 不同），
 *       所以"改了没存上"只能靠落盘时间戳确认；调用方须自行保证路径可写
 *       ——configPath_ 是相对路径，取决于进程 cwd（见 runtime_paths.h）。
 */
void ConfigManager::saveUnsafe()
{
    QFile file(configPath_);
    if (file.open(QIODevice::WriteOnly)) {
        QJsonDocument doc(root_);
        file.write(doc.toJson(QJsonDocument::Indented));
        file.close();
    }
}

// ============================================================================
// 将当前配置保存到 JSON 文件
// ============================================================================
// 使用 Indented 格式写入，便于人工阅读和编辑
// ============================================================================
void ConfigManager::save()
{
    std::lock_guard<std::mutex> lock(mutex_);
    saveUnsafe();
}

// ==========================================
// 通道配置（video.channels，数组下标即通道号 0..3）
// ==========================================
// 一个通道的全部信息都在这一条里（源/备注/开关/用哪些模型），
// 与 caichao 版 idge.json 的 channels[]+models[] 三层结构同构。
// 条目里的 "channel" 字段只给人看，程序按数组下标定通道。

// 取第 ch 个通道对象（0-based，越界返回空对象）
static QJsonObject channelObjectAt(const QJsonObject &video, int ch)
{
    const QJsonArray arr = video["channels"].toArray();
    if (ch >= 0 && ch < arr.size())
        return arr[ch].toObject();
    return QJsonObject();
}

// 写回第 ch 个通道对象，数组不足则补到 ch+1 项
// 补出来的项是"空源 + enabled=true + 无模型"，读起来就是"这一路还没配"，
// 不会覆盖已有条目；补齐是为了让 channels[] 恒为 4 项、下标恒等于通道号。
static void writeChannelObject(QJsonObject &video, int ch, const QJsonObject &obj)
{
    QJsonArray arr = video["channels"].toArray();
    while (arr.size() <= ch) {
        arr.append(QJsonObject{{"channel", arr.size()},
                              {"source_id", ""},
                              {"source", ""},
                              {"note", ""},
                              {"enabled", true},
                              {"models", QJsonArray{}}});
    }
    arr[ch] = obj;
    video["channels"] = arr;
}

// 修改第 ch 个通道对象的一个字段（读改写，保留其它字段）
static void patchChannelField(QJsonObject &root, int ch, const QString &key, const QJsonValue &val)
{
    QJsonObject video = root["video"].toObject();
    QJsonObject obj = channelObjectAt(video, ch);
    obj[key] = val;
    if (!obj.contains("channel")) obj["channel"] = ch;
    writeChannelObject(video, ch, obj);
    root["video"] = video;
}

/**
 * @brief 读第 ch 个通道的完整配置（一次加锁读全，避免多次取锁被中途改写）
 * @param ch 通道号，**0-based**；越界不报错，返回全默认值的 cfg
 *            （source/sourceId/note 空、enabled=true、modelIds 空）——
 *            调用方拿到"enabled=true 且 source 为空"要自己分辨"没配"与"配了空"
 * @note 解析口径：source/sourceId/每个模型 id 都做 trimmed()，所以 JSON 里写
 *       `" /dev/video0 "` 与 `"/dev/video0"` 等价；note **不 trim**（给人看的原文）。
 *       models 里的空串被直接丢弃，因此列表长度恒等于"真正配了的槽位数"，
 *       与 ModelRegistry::resolveChannelSlots() 的"紧凑下标=槽位号=0-based"约定对齐。
 * @note 缺键 = 该字段取结构体默认，enabled 缺键按 true（与 V1"四路全开"等价）。
 */
VideoChannelCfg ConfigManager::channelCfg(int ch) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const QJsonObject obj = channelObjectAt(root_["video"].toObject(), ch);
    VideoChannelCfg c;
    c.source = obj["source"].toString().trimmed();
    c.sourceId = obj["source_id"].toString().trimmed();
    c.note = obj["note"].toString();
    c.enabled = obj["enabled"].toBool(true);  // 缺键=打开，与 V1"四路全开"等价
    for (const auto &v : obj["models"].toArray()) {
        const QString id = v.toString().trimmed();
        if (!id.isEmpty())
            c.modelIds.append(id);
    }
    return c;
}

QString ConfigManager::channelSource(int ch) const
{
    // 返回**解析后**的地址：界面/解码器要的从来就是"这一路要开什么"，
    // 而不是"内联那个键里写了什么"。注册表引用与内联两种写法在这一步合流。
    return resolveChannelSource(ch).url;
}

void ConfigManager::setChannelSource(int ch, const QString &source)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ch < 0 || ch >= channelCount())
        return;
    // 写内联地址就是"这一路不用注册表了"，所以同时清掉 source_id。
    // 不清的话：用户在手填框里改了地址、保存、重启，画面还是旧流（source_id 优先），
    // 那就是一个"看起来能改其实不生效"的假开关。
    QJsonObject video = root_["video"].toObject();
    QJsonObject obj = channelObjectAt(video, ch);
    obj["source"] = source.trimmed();
    obj["source_id"] = QString();
    if (!obj.contains("channel")) obj["channel"] = ch;
    writeChannelObject(video, ch, obj);
    root_["video"] = video;
}

QString ConfigManager::channelSourceId(int ch) const
{
    return channelCfg(ch).sourceId;
}

QString ConfigManager::channelInlineSource(int ch) const
{
    return channelCfg(ch).source;
}

void ConfigManager::setChannelSourceId(int ch, const QString &id)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ch < 0 || ch >= channelCount())
        return;
    patchChannelField(root_, ch, QStringLiteral("source_id"), id.trimmed());
    // 内联 source **故意保留不动**：从"引用注册表"切回"不用注册表"时，
    // 现场原来手填的那个地址还在，不至于把老配置改成空串再靠记忆填回去。
}

QString ConfigManager::channelNote(int ch) const
{
    return channelCfg(ch).note;
}

void ConfigManager::setChannelNote(int ch, const QString &note)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ch < 0 || ch >= channelCount())
        return;
    patchChannelField(root_, ch, QStringLiteral("note"), note);
}

bool ConfigManager::channelEnabled(int ch) const
{
    return channelCfg(ch).enabled;
}

void ConfigManager::setChannelEnabled(int ch, bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ch < 0 || ch >= channelCount())
        return;
    patchChannelField(root_, ch, QStringLiteral("enabled"), enabled);
}

QStringList ConfigManager::channelModelIds(int ch) const
{
    return channelCfg(ch).modelIds;
}

/**
 * @brief 写第 ch 个通道的有序模型库 id 列表
 * @param ch 0-based；越界直接 return（不改内存，也不报错）
 * @param ids 顺序即级联槽位顺序（= result_id = 画面上的 M1/M2/…）
 * @note 写入前逐个 trimmed 并**丢空串 + 去重**（保留首次出现的顺序）：
 *       空串会在解析时变成一个"什么都不指向"的槽位，重复 id 会让同一个模型
 *       占两个推理任务、两份 NPU 上下文。所以这里传进去的条数可能小于 ids.size()，
 *       而槽位号按**过滤后**的下标算——不要拿调用方自己的下标去对报警里的 result_id。
 * @note 只改内存；本函数不判断"该路要不要重开"，那是设置页的职责
 *       （见 config/README.md 的"热更新分三级"）。
 */
void ConfigManager::setChannelModelIds(int ch, const QStringList &ids)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (ch < 0 || ch >= channelCount())
        return;
    QJsonArray arr;
    for (const QString &id : ids) {
        const QString one = id.trimmed();
        if (!one.isEmpty() && !arr.contains(one))
            arr.append(one);
    }
    patchChannelField(root_, ch, QStringLiteral("models"), arr);
}

// ==========================================
// 视频流注册表（video.sources[]，通道用 source_id 引用）
// ==========================================
// 为什么要一张表：同一只摄像头/同一路 rtsp 常被几路通道共用（监控四路 + 智能识别扫码 +
// 点名拍照），地址写在各通道里就是"改一处漏三处"；而分辨率/帧率/输入格式是**流的属性**，
// 通道该只管"挂哪些模型"。键名沿用 caichao 版 idge.json 的驼峰（hardwareDecode/inputFormat/
// timeoutMs/reconnectMs），两份配置能直接对拷；本仓 gb28181 段也是驼峰，不算新规矩。
//
// 三层兜底（resolveChannelSource）：source_id 命中注册表 → 用它；否则内联 source 非空 →
// 按老语义当地址用；都没有 → 空 = 界面显示"视频待接入"。所以现网两份 config.json
// （root 与 build 目录，值都是内联地址）不改也照跑。
// 键名与 JSON 的映射集中在这两个函数里，别在别处再拼一遍字符串字面量。
static VideoSourceCfg sourceFromJson(const QJsonObject &o)
{
    VideoSourceCfg s;
    s.id = o["id"].toString().trimmed();
    s.type = o["type"].toString().trimmed().toLower();
    // "mp4" 是本仓 gb28181 段用惯的词，与 caichao 版的 "file" 同义，收成一个口径，
    // 免得解码器那边要同时认两种拼法。空串表示"没写类型"，由 URL 前缀嗅探（旧行为）。
    if (s.type == QLatin1String("mp4"))
        s.type = QStringLiteral("file");
    s.url = o["url"].toString().trimmed();
    s.note = o["note"].toString();
    s.width = o["width"].toInt(0);
    s.height = o["height"].toInt(0);
    s.fps = o["fps"].toInt(0);
    s.inputFormat = o["inputFormat"].toString().trimmed();
    s.loop = o["loop"].toBool(false);
    s.hardwareDecode = o["hardwareDecode"].toBool(true);   // 缺键 = 硬解，与改动前一致
    s.timeoutMs = o["timeoutMs"].toInt(0);
    s.reconnectMs = o["reconnectMs"].toInt(0);
    return s;
}

/**
 * @brief VideoSourceCfg → JSON 对象（sourceFromJson 的逆操作，键名与归一规则同源）
 * @note ⚠ 必须与 sourceFromJson() 的字段清单**一一对齐**：这里少写一个键，
 *       setVideoSource() 就会把它静默落掉（写盘是整条替换，不是 patch），
 *       下次 load 出来该字段回到默认值——表现是"设置页改过重开就没了"。
 * @note 不做归一：type 的 "mp4→file" 只在读侧做（见 sourceFromJson），所以本函数
 *       会把调用方传进来的原始 type 原样写盘；界面回填时看到的可能是 "mp4"。
 * @note id/url 的 trim 由调用方（setVideoSource）负责，这里不重复处理。
 */
static QJsonObject sourceToJson(const VideoSourceCfg &s)
{
    QJsonObject o;
    o["id"] = s.id;
    o["type"] = s.type;
    o["url"] = s.url;
    o["note"] = s.note;
    o["width"] = s.width;
    o["height"] = s.height;
    o["fps"] = s.fps;
    o["inputFormat"] = s.inputFormat;
    o["loop"] = s.loop;
    o["hardwareDecode"] = s.hardwareDecode;
    o["timeoutMs"] = s.timeoutMs;
    o["reconnectMs"] = s.reconnectMs;
    return o;
}

// 取 video.sources 数组（不加锁，供已持锁的成员函数复用）
static QJsonArray sourcesArray(const QJsonObject &root)
{
    return root["video"].toObject()["sources"].toArray();
}

// 取国标那份流列表，判据与 Gb28181Config::load() 一致：顶层 streams 非空就用它，
// 空了才看 gb28181.streams。两边都数会把同一批流计两遍。
static QJsonArray gbStreamsArray(const QJsonObject &root)
{
    const QJsonArray top = root["streams"].toArray();
    if (!top.isEmpty())
        return top;
    return root["gb28181"].toObject()["streams"].toArray();
}

static void writeSourcesArray(QJsonObject &root, const QJsonArray &arr)
{
    QJsonObject video = root["video"].toObject();
    video["sources"] = arr;
    root["video"] = video;
}

/**
 * @brief 注册表里出现过的 id 列表（数组顺序即界面下拉顺序）
 * @return **可能含重复 id**：手改 JSON 留下同 id 两条时这里就出两条。
 *         去重收口在 setVideoSource()（只认第一条），所以下拉框用本函数拼模型
 *         要自己防重；id 为空的条目被直接跳过，不出现在列表里。
 */
QStringList ConfigManager::videoSourceIds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QStringList ids;
    for (const auto &v : sourcesArray(root_)) {
        const QString id = v.toObject()["id"].toString().trimmed();
        if (!id.isEmpty())
            ids.append(id);
    }
    return ids;
}

/**
 * @brief 注册表全部条目（数组顺序即界面下拉顺序）
 * @return **id 为空的条目被直接跳过**，不出现在结果里（也没有"未命名"占位），
 *         所以 out.size() 可能小于 JSON 里 sources 数组的长度；
 *         同 id 的重复条目会**都**返回（去重只在 setVideoSource 写侧做）
 */
QList<VideoSourceCfg> ConfigManager::videoSources() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QList<VideoSourceCfg> out;
    for (const auto &v : sourcesArray(root_)) {
        const VideoSourceCfg s = sourceFromJson(v.toObject());
        if (!s.id.isEmpty())
            out.append(s);
    }
    return out;
}

VideoSourceCfg ConfigManager::videoSource(const QString &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return videoSourceInRoot(root_, id);
}

VideoSourceCfg ConfigManager::videoSourceFromJson(const QJsonObject &o)
{
    return sourceFromJson(o);
}

VideoSourceCfg ConfigManager::videoSourceInRoot(const QJsonObject &root, const QString &id)
{
    if (id.isEmpty())
        return VideoSourceCfg();
    for (const auto &v : sourcesArray(root)) {
        const VideoSourceCfg s = sourceFromJson(v.toObject());
        if (s.id == id)
            return s;
    }
    // 悬空 id 不回退成"随便挑一条"：调用方拿到空 cfg（id 与 url 都空）就该报错或跳过，
    // 与 resolveChannelSource() 那条"悬空不 fallback 内联"的口径同一判据。
    return VideoSourceCfg();
}

bool ConfigManager::setVideoSource(const VideoSourceCfg &source)
{
    const QString id = source.id.trimmed();
    const QString url = source.url.trimmed();
    // 主键或地址为空就不写：宁可配置不动，也不要造出一条"有 id 没 url"的流——
    // 那种条目被通道引用后解析得出空地址，界面就是无声黑屏，比报错难查。
    if (id.isEmpty() || url.isEmpty())
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    VideoSourceCfg normalized = source;
    normalized.id = id;
    normalized.url = url;

    QJsonArray arr = sourcesArray(root_);
    QJsonArray kept;
    bool replaced = false;
    for (const auto &v : arr) {
        const QString one = v.toObject()["id"].toString().trimmed();
        if (one == id) {
            // 手改 JSON 很容易留下同 id 两条；只保留第一条的**替换结果**，
            // 否则"命中哪条"取决于数组顺序，同样的配置两个进程跑出两样行为。
            if (!replaced) {
                kept.append(sourceToJson(normalized));
                replaced = true;
            }
            continue;
        }
        kept.append(v);
    }
    if (!replaced)
        kept.append(sourceToJson(normalized));
    writeSourcesArray(root_, kept);
    return true;
}

void ConfigManager::removeVideoSource(const QString &id)
{
    if (id.isEmpty())
        return;
    std::lock_guard<std::mutex> lock(mutex_);

    QJsonArray arr = sourcesArray(root_);
    QJsonArray kept;
    for (const auto &v : arr) {
        if (v.toObject()["id"].toString().trimmed() != id)
            kept.append(v);
    }
    writeSourcesArray(root_, kept);

    // 引用一起清（与 removeModelParam 同一政策）：只删条目就会留下悬空 id，
    // 而悬空 id 的通道是"不打开 + 一条告警"，现场只看画面容易当成摄像头坏了。
    QJsonObject video = root_["video"].toObject();
    QJsonArray channels = video["channels"].toArray();
    for (int i = 0; i < channels.size(); ++i) {
        QJsonObject o = channels[i].toObject();
        if (o["source_id"].toString().trimmed() == id) {
            o["source_id"] = QString();
            channels[i] = o;
        }
    }
    video["channels"] = channels;
    root_["video"] = video;
}

/**
 * @brief 有多少处正在引用这条流（删之前给界面确认用）
 * @param id 流 id；空串直接返回 0（不遍历）
 * @return 引用处数量，两类消费者合并计数：video.channels[].source_id
 *         + 国标流列表里的 source_id
 * @note 国标那半边的取段判据与 Gb28181Config::load() **同一条**（见 gbStreamsArray：
 *       顶层 streams 非空就用它，否则才看 gb28181.streams），两处都数会把同一批流
 *       计两遍，让删除守卫报出"被引用 2 次"这种根本对不上的数。
 * @note 只做字符串相等比较（trim 后），不解析 gb28181 段的 enabled——
 *       一条禁用的国标流仍然算在引用，这是刻意的：它一旦在平台侧被启用就会立刻悬空。
 */
int ConfigManager::videoSourceReferrerCount(const QString &id) const
{
    if (id.isEmpty())
        return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    int n = 0;
    for (const auto &v : root_["video"].toObject()["channels"].toArray()) {
        if (v.toObject()["source_id"].toString().trimmed() == id)
            ++n;
    }
    for (const auto &v : gbStreamsArray(root_)) {
        if (v.toObject()["source_id"].toString().trimmed() == id)
            ++n;
    }
    return n;
}

VideoSourceCfg ConfigManager::resolveChannelSource(int ch) const
{
    // 不加本函数自己的锁：channelCfg()/videoSource() 各自持锁，这里再套一层就是死锁
    // （std::mutex 不可重入，见 ConfigManager.h 顶部"重入死锁"那条教训）。
    const VideoChannelCfg c = channelCfg(ch);

    if (!c.sourceId.isEmpty()) {
        const VideoSourceCfg s = videoSource(c.sourceId);
        if (!s.url.isEmpty())
            return s;
        // 悬空 id **不回退**到内联 source：那等于"你以为选的是 A，其实偷偷用了旧地址"，
        // 排查的人会盯着 A 看半天。宁可这一路不开，也要把这条告警留在日志里。
        qWarning() << "[ConfigManager] 通道" << ch << "引用的视频流 id 不在 video.sources 里:"
                   << c.sourceId << "—— 这一路不打开（请在设置页重新选流）";
        return VideoSourceCfg();
    }

    if (!c.source.isEmpty()) {
        // 老口径：内联地址直接当源。**type 故意留空** = 交给解码器按 url 前缀嗅探
        // （/dev/video → 摄像头，其余交给 FFmpeg 探容器），行为与加注册表之前逐字一致。
        VideoSourceCfg s;
        s.url = c.source;
        s.note = c.note;
        return s;
    }
    return VideoSourceCfg();
}

// ==========================================
// 模型参数表（video.models，键 = 模型库目录名）
// ==========================================
// 报警类别是"模型的参数"：同一个模型在任何通道、任何槽位触发同一类别。
// 键就是 model/library/ 下的目录名，因此配置里不再有 .rknn / labels.txt
// 路径字符串——路径/标签/类别名仍以模型库为唯一事实源（ModelRegistry 扫描得到），
// 仓库搬迁、换库、重命名目录都不需要重配这份 JSON。

static QJsonObject videoModelsObject(const QJsonObject &root)
{
    return root["video"].toObject()["models"].toObject();
}

// 把一个参数条目解析成结构体（不加锁，供已持锁的成员函数复用）
static VideoModelParam parseModelParam(const QJsonObject &models, const QString &libId)
{
    const QJsonObject o = models[libId].toObject();
    VideoModelParam p;
    p.alarmClass = o["alarm_class"].toString().trimmed();
    p.label = o["label"].toString().trimmed();
    p.note = o["note"].toString();
    // 必须先判键：QJsonValue 缺键时 toDouble() 返回 0.0，而 0.0 在阈值上是"看得见的合法值"，
    // 不判就会把"未配置"和"手滑写成 0"混成一件事（后者会让 NPU 后处理的预筛彻底失效）。
    if (o.contains(QStringLiteral("conf_threshold")))
        p.confThreshold = static_cast<float>(o["conf_threshold"].toDouble(-1.0));
    return p;
}

// 写一个字段；条目不存在就建（建了即保留，不因字段被清空而消失，
// 免得"清空备注"顺手把整个模型从参数表里删掉）
static void patchModelParamField(QJsonObject &root, const QString &libId,
                                 const QString &key, const QString &val)
{
    if (libId.trimmed().isEmpty())
        return;
    QJsonObject video = root["video"].toObject();
    QJsonObject models = video["models"].toObject();
    QJsonObject o = models[libId].toObject();
    o[key] = val;
    models[libId] = o;
    video["models"] = models;
    root["video"] = video;
}

// 数值版：conf_threshold 这类键不能用上面那个只吃 QString 的 patch
static void patchModelParamNumber(QJsonObject &root, const QString &libId,
                                  const QString &key, qreal val)
{
    if (libId.trimmed().isEmpty())
        return;
    QJsonObject video = root["video"].toObject();
    QJsonObject models = video["models"].toObject();
    QJsonObject o = models[libId].toObject();
    o[key] = val;
    models[libId] = o;
    video["models"] = models;
    root["video"] = video;
}

// 只删一个键（「跟随全局」= 这个键不存在）。不能拿 removeModelParam 代替：
// 那会连 alarm_class 一起抹掉，并把该 id 从每条通道列表里摘出去。
static void eraseModelParamField(QJsonObject &root, const QString &libId, const QString &key)
{
    if (libId.trimmed().isEmpty())
        return;
    QJsonObject video = root["video"].toObject();
    QJsonObject models = video["models"].toObject();
    QJsonObject o = models[libId].toObject();
    if (!o.contains(key))
        return;                       // 本来就缺键：不建空条目，免得参数表多出幽灵行
    o.remove(key);
    models[libId] = o;
    video["models"] = models;
    root["video"] = video;
}

/**
 * @brief 参数表里出现过的库 id
 * @return 按 key **升序**（QJsonObject 内部就是有序 map，与 JSON 里的书写顺序无关），
 *         所以设置页展示的模型顺序不等于现场添加顺序；video 段或 models 段不存在时返回空
 */
QStringList ConfigManager::modelParamKeys() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return videoModelsObject(root_).keys();
}

VideoModelParam ConfigManager::modelParam(const QString &libId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return parseModelParam(videoModelsObject(root_), libId);
}

QString ConfigManager::modelAlarmClass(const QString &libId) const
{
    return modelParam(libId).alarmClass;
}

void ConfigManager::setModelAlarmClass(const QString &libId, const QString &className)
{
    std::lock_guard<std::mutex> lock(mutex_);
    patchModelParamField(root_, libId, QStringLiteral("alarm_class"), className.trimmed());
}

void ConfigManager::setModelLabel(const QString &libId, const QString &label)
{
    std::lock_guard<std::mutex> lock(mutex_);
    patchModelParamField(root_, libId, QStringLiteral("label"), label.trimmed());
}

void ConfigManager::setModelNote(const QString &libId, const QString &note)
{
    std::lock_guard<std::mutex> lock(mutex_);
    patchModelParamField(root_, libId, QStringLiteral("note"), note);
}

float ConfigManager::modelConfThreshold(const QString &libId) const
{
    return modelParam(libId).confThreshold;   // 负数 = 未配置
}

void ConfigManager::setModelConfThreshold(const QString &libId, qreal val)
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 下限 0.05 是安全闸不是审美：量化输出的模型把阈值打到 u8 网格上比较
    // （yolo11_model.hpp 的 process_u8/process_i8），阈值到 0 会让 score_sum 预筛
    // 整体失效、validCount 爆到上限，随后是 O(N²) 的 NMS——推断线程冻死，
    // 画面卡住且没有任何看门狗会拉它起来。
    patchModelParamNumber(root_, libId, QStringLiteral("conf_threshold"),
                          qBound(0.05, val, 1.0));
}

void ConfigManager::clearModelConfThreshold(const QString &libId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    eraseModelParamField(root_, libId, QStringLiteral("conf_threshold"));
}

void ConfigManager::removeModelParam(const QString &libId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    const QString id = libId.trimmed();
    if (id.isEmpty())
        return;

    QJsonObject video = root_["video"].toObject();
    QJsonObject models = video["models"].toObject();
    models.remove(id);
    video["models"] = models;

    // 各通道列表里一并剔除：只删条目不删引用，会留下"通道在用、参数表没有"的悬空槽位
    QJsonArray chans = video["channels"].toArray();
    for (int i = 0; i < chans.size(); ++i) {
        QJsonObject o = chans[i].toObject();
        QJsonArray arr = o["models"].toArray();
        bool hit = false;
        for (int k = arr.size() - 1; k >= 0; --k) {
            if (arr[k].toString() == id) { arr.removeAt(k); hit = true; }
        }
        if (hit) { o["models"] = arr; chans[i] = o; }
    }
    video["channels"] = chans;
    root_["video"] = video;
}

QList<int> ConfigManager::modelParamUsers(const QString &libId) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const QString id = libId.trimmed();
    QList<int> users;
    const QJsonArray arr = root_["video"].toObject()["channels"].toArray();
    for (int i = 0; i < arr.size(); ++i) {
        if (arr[i].toObject()["models"].toArray().contains(id))
            users.append(i);
    }
    return users;
}

// ==========================================
// 检测参数配置
// ==========================================

// ============================================================================
// 获取置信度阈值
// ============================================================================
// 阈值范围 0.0 ~ 1.0，默认 0.25
// 只有置信度高于此阈值的检测结果才会被输出
//
// 调低：检测更多目标，但误检增多
// 调高：检测更少目标，但漏检增多
// ============================================================================
/**
 * @brief 全局置信度阈值，返回**缓存值** confThreshold_
 * @note 实际取值区间是 [0.05, 1.0]，由 setConfThreshold() 与
 *       ModelRegistry::resolveConfThreshold() **两处**夹出来（setter 挡 UI，
 *       resolver 挡"手改 JSON 后直接 load"那条路）。下限 0.05 是安全闸不是审美：
 *       量化模型把阈值打到 u8 网格上比较，0 会让 score_sum 预筛整体失效、
 *       候选爆到上限，接着是 O(N²) NMS，推断线程冻死且没有看门狗。
 * @note 本函数读的是缓存不是 root_：绕过 setter 直接改 detect.conf_threshold，
 *       这里不会变（缓存只在 load() 末尾的 updateCache() 刷一次）。
 * @note 它是回落链的**底座**：真正生效的每模型阈值请读
 *       ModelRegistry::resolveConfThreshold()，别直接拿本函数当"生效值"。
 */
double ConfigManager::confThreshold() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return confThreshold_;  // 返回缓存值，避免每次反序列化 JSON
}

// 设置置信度阈值
void ConfigManager::setConfThreshold(double val)
{
    std::lock_guard<std::mutex> lock(mutex_);
    val = qBound(0.05, val, 1.0);  // 下限 0.05：0 会让量化模型的预筛整体失效（见 setModelConfThreshold）
    QJsonObject detect = root_["detect"].toObject();
    detect["conf_threshold"] = val;
    root_["detect"] = detect;
    confThreshold_ = val;  // 更新缓存
}

// ============================================================================
// 获取 NMS（非极大值抑制）阈值
// ============================================================================
// 阈值范围 0.0 ~ 1.0，默认 0.45
// 用于消除重叠的检测框
//
// NMS 过程：
//   1. 按置信度排序所有检测框
//   2. 选择置信度最高的框
//   3. 删除与其 IoU（交并比）大于阈值的其他框
//   4. 重复直到所有框都被处理
//
// 调低：更激进地合并重叠框
// 调高：保留更多重叠框
// ============================================================================
/**
 * @brief 全局 NMS 阈值，返回缓存值 nmsThreshold_；**只有全局一档**，无每模型覆盖
 * @note 不设下限（setter 只夹 [0.0,1.0]）：调低是"合并得更狠"，方向上是变少不是变多，
 *       不存在置信度那条"预筛失效→候选爆炸"的失败模式，所以故意不给它编一个假护栏。
 *       不加每模型覆盖同理：NMS 是 IoU 合并口径，跨模型基本一致。
 * @note 与 confThreshold() 一样读缓存，缓存只在 load() 末尾刷一次。
 */
double ConfigManager::nmsThreshold() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return nmsThreshold_;  // 返回缓存值
}

// 设置 NMS 阈值
void ConfigManager::setNmsThreshold(double val)
{
    std::lock_guard<std::mutex> lock(mutex_);
    val = qBound(0.0, val, 1.0);  // 限制范围 0.0 ~ 1.0
    QJsonObject detect = root_["detect"].toObject();
    detect["nms_threshold"] = val;
    root_["detect"] = detect;
    nmsThreshold_ = val;  // 更新缓存
}

// ============================================================================
// 电子围栏配置
// ============================================================================
// config.json 中的 geofence 段格式：
// {
//   "geofence": {
//     "enabled": true,                    // 是否启用围栏检测
//     "mode": "inside_alarm",             // 【退役键】报警规则改由每个形状的 role 决定。
//                                         // 运行期无人读它；只在 FenceManager::loadFromConfig
//                                         // 里给没有 role 的老形状补一次角色，之后固定写回
//                                         // 一个占位值。新配置不必再管这个键。
//     "channels": {                       // 各通道围栏配置
//       "0": {                            // 通道 0（通道 1）
//         "shapes": [                     // 围栏形状列表
//           {
//             "type": "rectangle",        // 形状类型："rectangle"/"polygon"
//             "role": "forbidden",        // 角色："forbidden"=禁区（进去就报）
//                                         //       "workzone"=岗区域（在外面就报）
//                                         // 缺键→按上面的 mode 推断；认不出→forbidden
//             "points": [[100,50],[300,200]]  // 顶点坐标（归一化内容坐标 0..10000）
//           }
//         ]
//       }
//     },
//     "alarmClass": "person",             // 触发围栏报警的检测类别（单值；围栏不属于
//                                         // 级联模型，判定只看"这一路里是不是人"）
//     "throttleSeconds": 10               // 围栏报警节流下限：每通道每【角色】每类别窗口内只出一条。
//                                         // "一个人站进去只报一次"是解码器的进入沿事件表决定的，
//                                         // 这个窗口只兜住沿抖动/遮挡重匹配/多人同时违规。
//                                         // 分角色是必须的：否则闯入后 10 秒内的离岗会被吞掉
//   }
// }
//
// 坐标系说明：
//   - channels.<ch>.coord 标记形状顶点的坐标空间："norm"=归一化视频内容坐标
//     （0..10000，横轴除帧宽、纵轴除帧高），本版本读写只用这一种；
//     缺键或 "pixel" 是归一化之前的 widget 像素数据，由 FenceManager::loadFromConfig
//     就地换算并改写标记（换算口径见该函数注释）。
//   - 判定端不需要 widget 几何，overlay_w/h 已退役，只在换算那一次被当分母读。
// ============================================================================

/**
 * @brief 检查电子围栏是否启用
 * @return: true=启用，false=禁用（默认 false）
 */
bool ConfigManager::geofenceEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["enabled"].toBool(false);
}

/**
 * @brief 设置电子围栏启用状态
 * @param enabled: true=启用围栏检测，false=禁用
 */
void ConfigManager::setGeofenceEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["enabled"] = enabled;
    root_["geofence"] = gf;
}

/**
 * @brief 读退役键 geofence.mode
 * @return: "inside_alarm"（默认）或 "outside_alarm"
 *
 * 运行期已无判断读它 —— 报警规则现在是每个围栏形状自带的 role。
 * 唯一的消费者是 FenceManager::loadFromConfig()：给 role 缺失的老形状补角色
 * （outside_alarm → workzone，其余 → forbidden）。
 */
QString ConfigManager::geofenceMode() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["mode"].toString("inside_alarm");
}

/**
 * @brief 写退役键 geofence.mode（保留键本身，值不再有语义）
 * @param mode: 由调用方给出；FenceManager 固定写 "inside_alarm"
 */
void ConfigManager::setGeofenceMode(const QString &mode)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["mode"] = mode;
    root_["geofence"] = gf;
}

/**
 * @brief 获取所有通道的围栏配置
 * @return: QJsonObject，key 为通道号字符串（"0","1","2","3"），value 为该通道的围栏形状列表
 */
QJsonObject ConfigManager::geofenceChannels() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["channels"].toObject();
}

/**
 * @brief 设置所有通道的围栏配置
 * @param channels: QJsonObject，key 为通道号字符串，value 为围栏形状列表
 */
void ConfigManager::setGeofenceChannels(const QJsonObject &channels)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["channels"] = channels;
    root_["geofence"] = gf;
}

/**
 * @brief 获取围栏触发类别（单值）
 * @return 类别名，缺省/为空时返回 "person"
 *
 * 围栏报警不属于级联模型的任何一个槽位，判定只看"这一路里是不是人"，
 * 所以这里是一个类别名而不是列表。留空回落 "person"：语义只有一种
 * （按这个类别进围栏几何判定），不存在"空列表=不过滤"那种会把
 * helmet/vest 也报成入侵的坑。
 */
QString ConfigManager::geofenceAlarmClass() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const QString cls =
        root_["geofence"].toObject()["alarmClass"].toString().trimmed();
    return cls.isEmpty() ? QStringLiteral("person") : cls;
}

/** @brief 设置围栏触发类别（传空串则回落 "person"） */
void ConfigManager::setGeofenceAlarmClass(const QString &cls)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["alarmClass"] = cls.trimmed().isEmpty() ? QStringLiteral("person") : cls.trimmed();
    root_["geofence"] = gf;
}

/**
 * @brief 围栏报警节流窗口（秒），缺省 10
 * @note 缺键与"写了但类型不对"都会得到 10（QJsonValue::toInt(10)）；
 *       本 getter **不夹区间**，负数会被原样透传给 AlarmManager::ingestFenceAlarm——
 *       想关节流请去关 geofence.enabled，别指望写 0 或负数。
 */
int ConfigManager::geofenceThrottleSeconds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["geofence"].toObject()["throttleSeconds"].toInt(10);
}

/**
 * @brief 写围栏节流窗口
 * @param seconds >0 时原样写入；**<=0 一律写成 10**，不是"存 0 表示不节流"。
 *        想缩短到 1 秒就传 1；传 0 得到的还是 10（与本键的缺省值同一个数，
 *        刻意不给"关掉"这个语义，因为 0 秒节流 = 每帧一条报警，那是事故不是配置）。
 */
void ConfigManager::setGeofenceThrottleSeconds(int seconds)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject gf = root_["geofence"].toObject();
    gf["throttleSeconds"] = seconds > 0 ? seconds : 10;
    root_["geofence"] = gf;
}

// ============================================================================
// 数据库配置
// ============================================================================
// config.json 中的 database 段格式：
// {
//   "database": {
//     "storeDetections": true,        // 是否将报警相关的检测数据存入 detections 表
//     "detectionRetentionDays": 30    // 检测数据保留天数（超过自动清理）
//   }
// }
// ============================================================================

/**
 * @brief 是否启用检测数据存储
 * @return: true=将报警相关的检测数据存入 detections 表，false=只存 alarms 表
 */
bool ConfigManager::storeDetections() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return storeDetections_;  // 返回缓存值
}

/**
 * @brief 设置是否启用检测数据存储
 * @param enabled: true=启用，false=禁用
 */
void ConfigManager::setStoreDetections(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject db = root_["database"].toObject();
    db["storeDetections"] = enabled;
    root_["database"] = db;
    storeDetections_ = enabled;  // 更新缓存
}

/**
 * @brief 获取检测数据保留天数
 * @return: 保留天数（默认 30 天）
 */
int ConfigManager::detectionRetentionDays() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return detectionRetentionDays_;  // 返回缓存值
}

/**
 * @brief 设置检测数据保留天数
 * @param days: 保留天数
 */
void ConfigManager::setDetectionRetentionDays(int days)
{
    std::lock_guard<std::mutex> lock(mutex_);
    days = qBound(1, days, 365);  // 限制范围 1 ~ 365 天
    QJsonObject db = root_["database"].toObject();
    db["detectionRetentionDays"] = days;
    root_["database"] = db;
    detectionRetentionDays_ = days;  // 更新缓存
}

// ============================================================================
// recording 段：报警视频环形缓冲录制（硬件编码）
// ----------------------------------------------------------------------------
// 均为只读 getter：录制参数在解码器启动时一次性注入 VideoRecorder，
// 运行期改动不会重建编码器，因此不提供 setter（避免"改了不生效"的假接口）。
// 读取内存 root_，缺键回退到 makeRecordingDefaults() 的同一份默认值。
// ============================================================================
/**
 * @brief recording 段的取值辅助（本段五个 getter 都走它）
 * @note 每次现读 root_、不做缓存，所以与 updateCache() 那四个键不同：
 *       这里不存在"改了 JSON 但 getter 还是旧值"的窗口，只有"改了内存没 save"的窗口。
 */
static QJsonObject recordingSection(const QJsonObject &root)
{
    return root["recording"].toObject();
}

/** @brief 是否开录制（缺键默认 **true**：报警必带片段是需求验收项，见 README §三①） */
bool ConfigManager::recordingEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return recordingSection(root_).value("enabled").toBool(true);
}

/**
 * @brief 编码器选择，返回值**只可能是 "h264" 或 "hevc"**
 * @note 归一方向是刻意的：除 "h264" 外的一切取值（含拼错的 "H265"/"hevc_rkmpp"/空串）
 *       都落到 hevc。理由是这两个值最终只是拼成 "<codec>_rkmpp" 去查编码器，
 *       透传一个拼错的值会得到"打开编码器失败"这种很难归因的错误，
 *       而归一到主用档（带宽减半的那一个）最坏也只是"跟配置预期不一致"。
 *       ⚠ 大小写敏感："H264" 会被归一成 hevc，不是 h264。
 */
QString ConfigManager::recordingCodec() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QString codec = recordingSection(root_).value("codec").toString("hevc");
    return codec == "h264" ? codec : QStringLiteral("hevc");
}

/**
 * @brief 环形缓冲区时长（秒），qBound(1, 缺省 8, 60)
 * @note 【8 的来源】报警窗口是"前 3 + 后 2 = 5 秒"，但环按**包数**算
 *       （buffer_seconds × 流报帧率），入包节拍被 pacer 限到 ≤30fps，
 *       再叠加 gop_seconds=1 的关键帧对齐最坏吃掉前向 1 秒 —— 5 秒是零余量卡边，
 *       所以缺省给 8。⚠ config_example.json 模板里写的是 5，照模板装的新板拿到 5
 *       （模板值优先于缺省兜底）；这条不一致本轮只记录、未改值。
 * @note 上界 60 是内存护栏：环按**包数**存（maxPackets_ = bufferSeconds × cfg_.fps），
 *       60 秒 × 2Mbps ≈ 15MB/通道，四路常驻就是这个量级；下界 1 保证环里至少装得下
 *       一个 GOP（gop_seconds 缺省 1）。
 * @note ⚠ 兜底值有两处且**不同数**：本 getter 缺键回 8，VideoRecorder 构造函数里
 *       `bufferSeconds<=0` 回的是 5。后者只在手工拼 RecorderConfig（不经本 getter）时
 *       才命中——从 config.json 这条链进不来（本函数恒 ≥1）。改默认时两处一起看。
 * @note 与片段窗口的匹配只在 setRecordingConfig() 里做一次 qWarning 提示，
 *       不自动抬升——真配小了，报警片段的"前 3 秒"会被新包覆盖而截断。
 */
int ConfigManager::recordingBufferSeconds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1, recordingSection(root_).value("buffer_seconds").toInt(8), 60);
}

/**
 * @brief CBR 目标码率（bps），qBound(200000, 缺省 2000000, 16000000)
 * @note 区间即 config/README.md 里 recording.bitrate 声明的"20 万~1600 万"。
 * @note ⚠ **这一档是本仓唯一的护栏**：本段四个数值键里只有 bitrate 在
 *       VideoRecorder 构造函数里没有 <=0 兜底（那里只兜 fps/bufferSeconds/gopSeconds），
 *       值又被直接赋给 codecCtx_->bit_rate 且 rc_mode=1（CBR）——
 *       透传一个 0 或负数进去，CBR 拿到的目标码率就是无意义的，
 *       表现为编码器 init 失败→整条录制被 reset() 关掉，而日志只说 "recording disabled"。
 *       所以出格值必须在配置层就死掉，不能指望下游。
 * @note 默认 2 Mbps 是"1080p 片段够看清工服/人脸"的那一档，不是画质最优值；
 *       调它之前先想清楚环容量（buffer_seconds×fps 按包数存）与 eMMC 写带宽。
 */
int ConfigManager::recordingBitRate() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(200000, recordingSection(root_).value("bitrate").toInt(2000000), 16000000);
}

/**
 * @brief 关键帧间隔（秒），qBound(1, 缺省 1, 10)
 * @note 缺省取 1 的理由是**报警片段起点对齐误差 ≤1 秒**：dump 时窗口会自动对齐到
 *       窗口内第一个 IDR，GOP 越大截得越短（见 video_recorder.cpp 里
 *       `gop_size = fps * gopSeconds` 那行的注释）。
 * @note 下界 1 不是编码器约束：video_recorder.cpp 自己还会把 <=0 兜成 1，
 *       这里再夹一层是为了让"配置里看到的数"与"进编码器的数"是同一个，
 *       不然会出现 JSON 写 0、日志打 1 的两套口径。
 * @note 上界 10 的实际风险在别处：GOP 秒数若大于 buffer_seconds，环里可能
 *       一个关键帧都没有，片段起点就找不到 IDR 可对齐。
 */
int ConfigManager::recordingGopSeconds() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1, recordingSection(root_).value("gop_seconds").toInt(1), 10);
}

// ============================================================================
// alarm_uplink 段：报警上行（报警信息走 JSON；截图与片段按 media_transport 二选一，
//                  默认本机 http 文件服务出地址，另一档是 WS 二进制分块帧）
// ----------------------------------------------------------------------------
// 与 recording 段同为只读 getter：这些值在 AlarmUplink::attach() 时一次性读取，
// 运行期改动不重建状态机，因此不提供 setter（避免"改了不生效"的假接口）。
// http 服务本体更不允许运行期重开：ZLM 的 mk_env_init 是进程级一次、stop 后不能再起
//（见 src/media/server/http_file_service.h），所以端口改了要重启进程。
// 缺键回退到 makeAlarmUplinkDefaults() 的同一份默认值。
// ⚠ migrate 只补**整个缺失的 alarm_uplink 段**，不往已有段里加键 —— 现场老 config.json
//   永远看不到 media_transport/media_http_port/media_base_url 这三键，靠的就是上面的缺键回退。
// ============================================================================

/**
 * @brief 本段的共享取值辅助；下面十个 getter 全部"每次现读 root_ + qBound 夹区间"
 * @note ⚠ 本段**没有任何 setter**，且 migrate 只补整段缺失的 alarm_uplink
 *       （见 migrateAlarmUplinkDefaults 注释）。所以现场那台跑过一次的 config.json
 *       里永远不会自动出现 media_transport/media_http_port/media_base_url 三键，
 *       它们全靠这里的缺省值兜住——"改了 JSON 没生效"要区分是键没写进去还是没重启。
 */
static QJsonObject alarmUplinkSection(const QJsonObject &root)
{
    return root["alarm_uplink"].toObject();
}

/** @brief 总开关（缺省 **false**：链路明文零鉴权且是往外发数据，必须人来打开；
 *         false 时 AlarmUplink 不订阅信号、不建线程、零分配） */
bool ConfigManager::alarmUplinkEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return alarmUplinkSection(root_).value("enabled").toBool(false);
}

/** @brief 第一段（报警信息）是否申报抓拍图（缺省 true；false = 只发文字，不发媒体计划） */
bool ConfigManager::alarmUplinkSendSnapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return alarmUplinkSection(root_).value("send_snapshot").toBool(true);
}

/**
 * @brief 第二段是否申报视频片段（缺省 true）
 * @note 本 getter **不**判断 recording.enabled。"录制关了就自动降级为不发片段"这件事
 *       发生在上行侧（见 config/README.md 的 alarm_uplink.send_clip 一条），
 *       base 域不去读另一个模块的开关，否则这里就成了第二个真值来源。
 */
bool ConfigManager::alarmUplinkSendClip() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return alarmUplinkSection(root_).value("send_clip").toBool(true);
}

/**
 * @brief ws_binary 档的单个二进制帧字节数，qBound(65536, 缺省 262144, 1048576)
 * @note 区间与默认值见 config/README.md（64KB~1MB、256KB）。默认 256KB 的依据是
 *       "Qt 出站按一次调用=一个整帧"的写法：分块大小就是帧大小，所以只有对端
 *       maxFrameSize 更小时才需要调小，不需要在代码里加切分逻辑。
 * @note 仅 media_transport="ws_binary" 时被读；url 档一次都不会用到这个值。
 *       夹区间的原因与 recording.bitrate 同一条：透传一个 0 或负数进去，
 *       下游切分循环会直接退化成死循环或空帧，而那不是本模块能看出来的错。
 */
int ConfigManager::alarmUplinkChunkSize() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(65536, alarmUplinkSection(root_).value("chunk_size").toInt(262144), 1048576);
}

/**
 * @brief 等 uplink_ack 的超时（毫秒），qBound(2000, 缺省 15000, 60000)
 * @note **只有 expect_ack=true 才有意义**，而当前对端不回 ack、expect_ack 出厂 false，
 *       所以这个值今天不影响任何行为——改它之前先确认 expect_ack 真的开了。
 * @note 下界 2000：局域网内一次往返远小于此，再短就是在本机也在忙的时候误判超时重投。
 */
int ConfigManager::alarmUplinkAckTimeoutMs() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(2000, alarmUplinkSection(root_).value("ack_timeout_ms").toInt(15000), 60000);
}

/**
 * @brief 同一项最多重投几次，qBound(1, 缺省 3, 10)
 * @note 下界 1 = "至少试一次"，0 次没有语义（等价于关掉上行，那该由 enabled 决定）；
 *       超过上限判 failed 并打 warning，不会无限重试。
 */
int ConfigManager::alarmUplinkMaxAttempts() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1, alarmUplinkSection(root_).value("max_attempts").toInt(3), 10);
}

/**
 * @brief 待发队列上限，qBound(1, 缺省 16, 64)
 * @note 满了**丢最旧**（与 ClipWriter/SnapWriter 同口径）。上界 64 是内存与
 *       "丢了也别丢太多"的折中：一条 alarm_info 只是 JSON，64 条不构成压力，
 *       但队列一深就说明链路早就断了，再攒下去只会发出一批过期报警。
 */
int ConfigManager::alarmUplinkMaxPending() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1, alarmUplinkSection(root_).value("max_pending").toInt(16), 64);
}

/** @brief 是否要求对端回 uplink_ack 才算完成（缺省 false：当前对端不回，开了会把每项都吊住） */
bool ConfigManager::alarmUplinkExpectAck() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return alarmUplinkSection(root_).value("expect_ack").toBool(false);
}

// 媒体交付方式。原样返回字符串，认不认得由 uplink 侧判（base 域不依赖 biz 域的字面量）。
// 缺键或非 "ws_binary" 的一切取值都按 "url" 处理：url 是本轮定的默认，
// 而把拼错的 "Ws_Binary"/"wsbinary" 也当默认，比"静默不发消息"更容易在日志里被发现。
QString ConfigManager::alarmUplinkMediaTransport() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const QString raw = alarmUplinkSection(root_).value("media_transport").toString("url").trimmed();
    return raw == QLatin1String("ws_binary") ? raw : QStringLiteral("url");
}

int ConfigManager::alarmUplinkMediaHttpPort() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 下界 1024 不是随手写的：板端 net.ipv4.ip_unprivileged_port_start=1024，
    // 给个 80 进去只会 bind 失败，然后现场排查人员对着"服务起不来"的日志一头雾水
    return qBound(1024, alarmUplinkSection(root_).value("media_http_port").toInt(8090), 65535);
}

QString ConfigManager::alarmUplinkMediaBaseUrl() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return alarmUplinkSection(root_).value("media_base_url").toString().trimmed();
}

// ============================================================================
// pad_server 段：Pad 接口 v1 的入站服务端（本机是 WS 服务端，Pad 连进来收报警）
// ----------------------------------------------------------------------------
// 与 alarm_uplink 同为只读 getter，不给 setter：监听口开开关关要走重启，
// 运行期"改了立刻生效"反而会把已连的 Pad 踢掉却看不出原因。
// ⚠ port 与 alarm_uplink.media_http_port（内嵌 http 文件服务）不是同一个口：
//   前者是 WS 入站（8900），后者是媒体字节出站（8090）。对方给的 A4 分配是
//   「8899 给 /api/ HTTP、WS 走 8900」，所以本机三个口各管一件事，谁也不抢谁。
// ============================================================================

/**
 * @brief pad_server 段的共享取值辅助（六个 getter 都是"现读 root_ + 缺键兜底"）
 * @note 全段**只有 getter**：监听口开开关关要走重启，运行期"改了立刻生效"只会让现场
 *       看不出 Pad 到底连的是哪个口（理由见本函数上方注释与 config/README.md）。
 *       加 setter 之前先解决"已连的 Pad 怎么办"，否则就是个假开关。
 */
static QJsonObject padServerSection(const QJsonObject &root)
{
    return root["pad_server"].toObject();
}

/** @brief 是否启用 Pad 入站服务端（缺省 **true**，2026-11-04 起；显式 false 才关。
 *         这是明文零鉴权的**入站**监听口，唯一的门是握手 Origin 白名单） */
bool ConfigManager::padServerEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 缺键 = true（2026-11-04 起）：本机用途就是被 Pad 连，默认开着；关掉要显式写 false。
    return padServerSection(root_).value("enabled").toBool(true);
}

/**
 * @brief Pad WS 监听端口，qBound(1024, 缺省 8900, 65535)
 * @note 下界 1024 与 alarmUplinkMediaHttpPort() 同一条理由：板端 uid 1000 +
 *       net.ipv4.ip_unprivileged_port_start=1024，特权口 bind 不上，挡在配置层。
 * @note A4（2026-11-04 对方答复）：**8899 归 `http://<host>:8899/api/`，WS 用 `ws://<host>:8900/`**。
 *       规约 §2.1 原文把两个都写成 8899 占位，在这套栈上做不到（`QWebSocketServer` 独占一只监听器），
 *       现在按拆口解决。媒体字节出站仍是 `alarm_uplink.media_http_port`（8090），第三个口，互不冲突。
 * @warning 现场 `data/config.json` 里若**显式写了 `"port": 8899`**，改默认值对它无效，
 *          WS 会继续占住对方分配给 `/api/` 的口——换口必须连那份配置一起改。
 */
int ConfigManager::padServerPort() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    // 与 media_http_port 同一条下界理由：1024 以下在本机 bind 不上，不如挡在配置层
    return qBound(1024, padServerSection(root_).value("port").toInt(8900), 65535);
}

/**
 * @brief qr_login 段（扫码登录校验档；缺段 = 全默认，即不比对本机当前计划）
 * @note §6.4 有效期与 §6.5 验签本轮不做（2026-11-04 裁定），所以这里只有一个键。
 *       将来补验签时，公钥随安装配置下发、**不能放进二维码**（§7）。
 */
static QJsonObject qrLoginSection(const QJsonObject &root)
{
    return root["qr_login"].toObject();
}

QString ConfigManager::qrLoginExpectedTaskId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qrLoginSection(root_).value("expected_task_id").toString().trimmed();
}

// ========== pad_server 的 setter（调试设置页用，2026-11-05） ==========
// 六个键一次写进同一段再 save()：界面点一次"保存并应用"要么全生效、要么全不生效。
// 范围键在这里 qBound，与 getter 同一条判据（下界 1024 的理由见 padServerPort）。
void ConfigManager::setPadServerEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject pad = padServerSection(root_);
    pad["enabled"] = enabled;
    root_["pad_server"] = pad;
}

void ConfigManager::setPadServerPort(int port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject pad = padServerSection(root_);
    pad["port"] = qBound(1024, port, 65535);
    root_["pad_server"] = pad;
}

void ConfigManager::setPadServerPath(const QString &path)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QString raw = path.trimmed();
    if (raw.isEmpty())
        raw = QStringLiteral("/");
    if (!raw.startsWith(QLatin1Char('/')))
        raw.prepend(QLatin1Char('/'));
    QJsonObject pad = padServerSection(root_);
    pad["ws_path"] = raw;
    root_["pad_server"] = pad;
}

void ConfigManager::setPadServerDeviceId(const QString &deviceId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject pad = padServerSection(root_);
    pad["device_id"] = deviceId.trimmed();
    root_["pad_server"] = pad;
}

void ConfigManager::setPadServerMediaBaseUrl(const QString &baseUrl)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject pad = padServerSection(root_);
    pad["media_base_url"] = baseUrl.trimmed();
    root_["pad_server"] = pad;
}

void ConfigManager::setPadServerSyncLimit(int limit)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject pad = padServerSection(root_);
    pad["sync_limit"] = qBound(1, limit, 5000);
    root_["pad_server"] = pad;
}

// ============================================================================
// pad_http 段：Pad 接口 v1 的 HTTP `/api/` 面（§4 处置 / §5 围栏 / §6 视频 / §7 设备）
// ----------------------------------------------------------------------------
// 与 pad_server 分开的理由不是"两个开关好看"，是**两台服务端生命周期不同**：
//   pad_server = QWebSocketServer，能关能开；pad_http = QTcpServer，也能关能开；
//   但两者端口不同、失败原因不同，混在一个开关里就会出现"想关报警面却把围栏写面也关了"。
// 三条 getter 的范围判据都写在这里而不是界面里，理由与 padServerPort 相同：
//   下界 1024（板端 uid 1000 + ip_unprivileged_port_start=1024，特权口 bind 不上），
//   上界由本函数 qBound，界面那侧的 QSpinBox 只是让它点不出越界的值，不是第二道判据。
// ============================================================================

static QJsonObject padHttpSection(const QJsonObject &root)
{
    return root["pad_http"].toObject();
}

/** @brief 是否启用 HTTP `/api/` 面（缺键 **true**，与 pad_server 同一天定案的口径） */
bool ConfigManager::padHttpEnabled() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return padHttpSection(root_).value("enabled").toBool(true);
}

/**
 * @brief HTTP 监听端口，qBound(1024, 缺省 8899, 65535)
 * @note 8899 是 A4（2026-11-04 对方答复）分给 `http://<host>:8899/api/` 的那个口。
 *       规约 §2.1 原文把 WS 与 `/api/` 都写成 8899 占位，那在这套栈上做不到
 *       （两只监听器不能同口），所以本机现在是三个口：
 *       8899=HTTP `/api/`、8900=Pad WS、8090=报警媒体静态文件。
 */
int ConfigManager::padHttpPort() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1024, padHttpSection(root_).value("port").toInt(8899), 65535);
}

/** @brief 请求正文上限（MB），qBound(1, 缺省 26, 100)。26 = §4.2 五张 5MB 照片 + JSON 余量 */
int ConfigManager::padHttpMaxBodyMb() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1, padHttpSection(root_).value("max_body_mb").toInt(26), 100);
}

/**
 * @brief liveUrl 是否保留 `user:pass@`，缺省 **false**（剥掉）
 * @note 为什么默认关：§6.1 让 Pad 直接播这个地址，于是它会出现在对端的日志、缓存与界面里。
 *       "局域网零鉴权"是 §2.3 认了的**这台设备**的口径，不等于把每台摄像头的口令也发出去。
 *       关掉之后 Pad 拿到的是无凭据地址，多半播不起来——那是可见的失败，比口令出门好处理。
 */
bool ConfigManager::padHttpExposeStreamCredentials() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return padHttpSection(root_).value("expose_stream_credentials").toBool(false);
}

/** @brief 围栏 apply 时是否渲染合成画面 PNG，缺省 true（§5.3 要求 apply 必返 frameUrl） */
bool ConfigManager::padHttpFenceFrames() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return padHttpSection(root_).value("fence_frames").toBool(true);
}

/**
 * @brief §4.1 的处置选项数组；**空数组 = 调用方用代码内置那两条**
 * @note 这里不校验元素形状：校验在 §4.1 的路由里做一次（那里能直接回 500/3001 并说清是哪条），
 *       在配置层校验就只能打一条 qWarning，现场对着配置文件看不出哪个键拼错了。
 */
QJsonArray ConfigManager::padHttpDisposeOptions() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return padHttpSection(root_).value("dispose_options").toArray();
}

void ConfigManager::setPadHttpEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject http = padHttpSection(root_);
    http["enabled"] = enabled;
    root_["pad_http"] = http;
}

void ConfigManager::setPadHttpPort(int port)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject http = padHttpSection(root_);
    http["port"] = qBound(1024, port, 65535);
    root_["pad_http"] = http;
}

void ConfigManager::setPadHttpMaxBodyMb(int megabytes)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject http = padHttpSection(root_);
    http["max_body_mb"] = qBound(1, megabytes, 100);
    root_["pad_http"] = http;
}

void ConfigManager::setPadHttpExposeStreamCredentials(bool expose)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject http = padHttpSection(root_);
    http["expose_stream_credentials"] = expose;
    root_["pad_http"] = http;
}

void ConfigManager::setPadHttpFenceFrames(bool render)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject http = padHttpSection(root_);
    http["fence_frames"] = render;
    root_["pad_http"] = http;
}

void ConfigManager::setPadHttpDisposeOptions(const QJsonArray &options)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject http = padHttpSection(root_);
    http["dispose_options"] = options;
    root_["pad_http"] = http;
}

/**
 * @brief Pad WS 路径，返回值**保证以 '/' 开头**（空串回落 "/"）
 * @note ⚠ 这个值只进日志。QWebSocketServer 不按路径筛连接，Pad 连哪条路径都能握手成功，
 *       所以写 "/ws" **不会**把别的路径挡掉——不要把它当访问控制来配。
 * @note 补前导 '/' 是为了日志与 v1 文档里的写法对得上（文档一律写带斜杠的路径）。
 */
QString ConfigManager::padServerPath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const QString raw = padServerSection(root_).value("ws_path").toString(QStringLiteral("/")).trimmed();
    if (raw.isEmpty()) {
        return QStringLiteral("/");
    }
    return raw.startsWith(QLatin1Char('/')) ? raw : QStringLiteral("/") + raw;
}

/**
 * @brief hello_ack.deviceId 的显式覆盖值
 * @return **空串 = 不覆盖**，由 pad::defaultDeviceId() 按 /etc/machine-id 前 12 位派生
 *         （本机实测 EDGE-1030A3F99414）。该键在报文里**一定出现**——v1 §13.1.3 规定必填，
 *         "留空"只是"用派生值"而不是"不发这个字段"。填了就赢。
 */
QString ConfigManager::padServerDeviceId() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return padServerSection(root_).value("device_id").toString().trimmed();
}

/**
 * @brief 证据 URL 前缀覆盖（空 = 复用内嵌 http 文件的自动地址）
 * @note 与 alarmUplinkMediaBaseUrl() 是两个独立键，读谁由消费者决定；
 *       都留空时两者都会指向同一个内嵌服务，写了哪个就以哪个为准。
 *       对端只能经 NAT/反代访问时必须写死——自动口径取的是本机网卡地址，够不到。
 */
QString ConfigManager::padServerMediaBaseUrl() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return padServerSection(root_).value("media_base_url").toString().trimmed();
}

/**
 * @brief 一次 sync_result 最多带几条，qBound(1, 缺省 500, 5000)
 * @note 500 与上界 5000 都是本仓自定的数（config/README.md：v1 没给分页规则，先设上限），
 *       **不是协议里的值**——将来 v1 补了分页口径要以协议为准重看这两个数。
 * @note 游标取"本次返回里的最大 update_time"，所以截断不会漏下一批：
 *       下界夹到 1 语义仍然正确，只是要多来回几次。
 */
int ConfigManager::padServerSyncLimit() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return qBound(1, padServerSection(root_).value("sync_limit").toInt(500), 5000);
}

// ==========================================
// WebSocket 配置
// ==========================================

/**
 * @brief Pad↔Aibox 客户端的服务器地址（如 "ws://192.168.1.100:8765"）
 * @return 缺键/未配返回**空串**，空串 = 整条链路不启动（这是"关"的唯一种写法）
 * @note ⚠ 只能填 `ws://`。`wss://` 在本机不可用：Qt 的 OpenSSL 符号缺口未适配
 *       （见根 README 第 11 条），填了会连不上而不是报"不支持 TLS"。
 *       因此这条链路是明文 + 零鉴权，配置层不做校验也不拦。
 * @note 本键是 websocket 段里**唯一有 setter** 的（调试设置页可直接编辑），
 *       另两个间隔键没有控件、只能改 JSON。
 */
QString ConfigManager::wsServerUrl() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["websocket"].toObject()["server_url"].toString();
}

/** @brief 写服务器地址；本函数不判空也不校验 scheme，空串同样能写进内存再 save() 落盘 */
void ConfigManager::setWsServerUrl(const QString &url)
{
    std::lock_guard<std::mutex> lock(mutex_);
    QJsonObject ws = root_["websocket"].toObject();
    ws["server_url"] = url;
    root_["websocket"] = ws;
}

/**
 * @brief 应用层心跳周期（秒），缺省 30
 * @note ⚠ **本键没有 qBound**（与 recording/alarm_uplink 那些数值键不同口径）：
 *       写 0 会得到 0，而消费侧是 `heartbeatTimer_->start(interval * 1000)`
 *       （ws_client.cpp:224），QTimer 传 0 就是连发 —— 等于把心跳变成忙循环。
 *       这里没夹是历史现状，要加区间请先想清楚"0 到底该是关心跳还是 1 秒"，
 *       别顺手写个 qBound 把语义改掉。
 * @note 无界面控件，只能改 JSON 后重启。
 */
int ConfigManager::wsHeartbeatInterval() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["websocket"].toObject()["heartbeat_interval"].toInt(30);
}

/**
 * @brief 非主动断开后的重连延时（秒），缺省 5
 * @note 与 wsHeartbeatInterval() 同一条"没夹区间"的现状：0 会走到
 *       `reconnectTimer_->start(0)`（ws_client.cpp:251），表现是断开后疯狂重连。
 *       只在 !stopping_（非用户主动停）时被读，主动 stop 不会重连。
 */
int ConfigManager::wsReconnectInterval() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return root_["websocket"].toObject()["reconnect_interval"].toInt(5);
}

// ========== 智能识别配置 ==========

QString ConfigManager::smartRecognitionCamera() const
{
    // 配置显式给出则以配置为准（可填设备节点、本地视频、RTSP，便于无摄像头自测）
    QString configured;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        configured = root_["smartRecognition"].toObject()["camera"].toString();
    }
    if (!configured.isEmpty()) return configured;

    // 缺省时实探测摄像头节点（MJPEG 优先，CSI/rkisp 的 NV12、YUYV 也算），
    // 避免写死编号在换硬件/重启后失效
    const QString probed = probeCameraNode();
    if (!probed.isEmpty()) return probed;

    qWarning() << "smartRecognition.camera is unset and no usable camera node was found;"
                  " falling back to /dev/video0";
    return QStringLiteral("/dev/video0");
}

// 智能识别/扫码摄像头的采集尺寸。默认值即 kDefaultCameraSize（"帧率优先"那一档）。
QString ConfigManager::smartRecognitionCameraSize() const
{
    QString configured;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        configured = root_["smartRecognition"].toObject()["camera_size"].toString();
    }
    if (configured.trimmed().isEmpty())
        return QString::fromLatin1(kDefaultCameraSize);

    // "WxH"，两轴都必须是 8 的倍数：本机 rkisp mainpath 的 stepwise 范围写的就是
    // step 8/8，给了非 8 倍数会被 S_FMT 拒掉，表现成"摄像头打不开"而不是一句尺寸不对。
    const QStringList parts = configured.trimmed().split(QLatin1Char('x'));
    bool okW = false;
    bool okH = false;
    const int w = parts.size() == 2 ? parts.at(0).trimmed().toInt(&okW) : -1;
    const int h = parts.size() == 2 ? parts.at(1).trimmed().toInt(&okH) : -1;
    if (!okW || !okH || w < 320 || w > 4224 || h < 240 || h > 3136) {
        qWarning() << "smartRecognition.camera_size 不可用:" << configured
                   << "应为 \"宽x高\"（宽 320..4224、高 240..3136）; 回退"
                   << kDefaultCameraSize;
        return QString::fromLatin1(kDefaultCameraSize);
    }

    const int alignedW = w & ~7;
    const int alignedH = h & ~7;
    if (alignedW != w || alignedH != h) {
        qWarning() << "smartRecognition.camera_size" << configured
                   << "不是 8 的倍数，已对齐到" << alignedW << 'x' << alignedH;
    }
    return QStringLiteral("%1x%2").arg(alignedW).arg(alignedH);
}
