// ============================================================================
// ModelRegistry.cpp - 模型库注册表实现
// ============================================================================
//
// 库目录结构（每个模型一个子目录，目录名即 id）：
//   model/library/
//     ├── yolo11n-coco/
//     │     ├── model.rknn
//     │     ├── labels.txt     # 一行一类，行序 = 模型输出的 cls_id
//     │     └── meta.json      # 元数据，缺失时扫描自动生成
//     └── ...
//
// meta.json 字段（与 plan/model_management.md §一"meta.json 字段"对齐）：
//   id / display_name / family / task / num_classes / classes /
//   input_size [w,h] / quant / version / sha256 / notes
//
// ============================================================================

#include "model_registry.h"
#include "ConfigManager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDate>
#include <QDateTime>
#include <QRegularExpression>
#include <QTextStream>
#include <QDebug>

#include <algorithm>

// 库内固定文件名
static const char *kModelFile = "model.rknn";
static const char *kLabelFile = "labels.txt";
static const char *kMetaFile  = "meta.json";

// ============================================================================
// 单例
// ============================================================================
ModelRegistry &ModelRegistry::instance()
{
    static ModelRegistry reg;
    return reg;
}

// ============================================================================
// 计算文件 sha256（流式读取，4.2~22MB 的 rknn 无压力）
// ============================================================================
static QString fileSha256(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f))
        return QString();
    return QString::fromLatin1(hash.result().toHex());
}

// ============================================================================
// 读取 labels.txt → 类别列表（去空行、去行尾注释）
// ============================================================================
static QStringList readLabelLines(const QString &labelPath)
{
    QStringList classes;
    QFile f(labelPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return classes;
    QTextStream ts(&f);
    while (!ts.atEnd()) {
        QString line = ts.readLine().trimmed();
        if (!line.isEmpty())
            classes.append(line);
    }
    return classes;
}

// ============================================================================
// 从目录名猜 family/task，如 "yolo11n-helmet" → family=yolo11, task=helmet
// ============================================================================
static void guessFamilyTask(const QString &id, QString &family, QString &task)
{
    static const QRegularExpression reFamily("^(yolo[0-9]+[a-z]?)");
    const QRegularExpressionMatch m = reFamily.match(id);
    if (m.hasMatch())
        family = m.captured(1);
    const int dash = id.indexOf('-');
    if (dash >= 0)
        task = id.mid(dash + 1);
    if (task.isEmpty())
        task = QStringLiteral("detection");
}

// ============================================================================
// meta.json → ModelMeta（字段缺失时给兜底值）
// ============================================================================
static ModelMeta parseMetaJson(const QJsonObject &o, const QString &id, const QString &dir)
{
    ModelMeta m;
    m.id = id;
    m.dir = dir;
    m.displayName = o["display_name"].toString(id);
    m.family = o["family"].toString();
    m.task = o["task"].toString();
    m.classes = [&] {
        QStringList list;
        for (const auto &v : o["classes"].toArray()) list.append(v.toString());
        return list;
    }();
    m.numClasses = o["num_classes"].toInt(m.classes.size());
    const QJsonArray size = o["input_size"].toArray();
    if (size.size() >= 2) {
        m.inputWidth = size[0].toInt(640);
        m.inputHeight = size[1].toInt(640);
    }
    m.quant = o["quant"].toString();
    m.version = o["version"].toString();
    m.sha256 = o["sha256"].toString();
    m.notes = o["notes"].toString();
    guessFamilyTask(id, m.family, m.task);  // meta 缺字段时用目录名兜底
    return m;
}

// ============================================================================
// 扫描库目录，重建注册表
// ============================================================================
int ModelRegistry::rescan(const QString &libraryDir)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.clear();
    }

    QDir lib(libraryDir);
    if (lib.exists()) {
        const QFileInfoList dirs = lib.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &d : dirs) {
            const QString dirPath = d.absoluteFilePath();
            if (QFile::exists(dirPath + "/" + kModelFile)) {
                loadEntryDir(dirPath);
                continue;
            }
            // "权重放进库目录却看不见"的头号原因是文件名没按约定写成 model.rknn，以前静默跳过。
            // 判定收窄到"有 labels.txt 且恰好一个 .rknn"：这样 ppocrv4-ocr 那种多权重目录
            // （det.rknn + rec.rknn，本就不走单模型约定）不会天天报假警告。
            const QStringList stray = QDir(dirPath).entryList({QStringLiteral("*.rknn")}, QDir::Files);
            if (stray.size() == 1 && stray.first() != QString(QLatin1String(kModelFile))
                && QFile::exists(dirPath + "/" + kLabelFile)) {
                qWarning() << "[ModelRegistry] 未收录" << d.fileName() << ": 目录里的权重叫"
                           << stray.first() << "，单模型库约定文件名固定为" << kModelFile
                           << "，请改名后重启（否则该模型不会出现在模型列表里）";
            }
        }
    } else {
        qInfo() << "[ModelRegistry] 模型库目录不存在:" << libraryDir;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    QStringList ids;
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it)
        ids.append(it.key());
    qInfo() << "[ModelRegistry] 已收录" << entries_.size() << "个模型:" << ids.join(", ");
    return entries_.size();
}

void ModelRegistry::loadEntryDir(const QString &dirPath)
{
    // 由 rescan() 在持锁外调用；此处只解析，不写 entries_
    QFileInfo dirInfo(dirPath);
    const QString id = dirInfo.fileName();
    const QString modelFile = dirPath + "/" + kModelFile;
    const QString labelFile = dirPath + "/" + kLabelFile;
    const QString metaFile = dirPath + "/" + kMetaFile;

    ModelMeta m;
    m.id = id;
    m.dir = dirInfo.absoluteFilePath();

    QFile mf(metaFile);
    if (mf.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(mf.readAll());
        mf.close();
        if (doc.isObject()) {
            m = parseMetaJson(doc.object(), id, dirInfo.absoluteFilePath());
        }
    }

    // labels.txt 始终以库内实际文件为准（meta 里的 classes 可能过期）
    const QStringList labels = readLabelLines(labelFile);
    if (!labels.isEmpty() && labels != m.classes) {
        m.classes = labels;
        m.numClasses = labels.size();
    } else if (m.numClasses == 0) {
        m.numClasses = m.classes.size();
    }

    // sha256 缺失则补算（同时用于 importModel 去重）
    if (m.sha256.isEmpty())
        m.sha256 = fileSha256(modelFile);

    if (m.displayName.isEmpty())
        m.displayName = id;
    guessFamilyTask(id, m.family, m.task);

    // meta.json 缺失或字段有更新时回写，方便人工查看/修订
    const bool metaMissingOrStale = !QFileInfo::exists(metaFile);
    if (metaMissingOrStale) {
        QJsonObject o;
        o["id"] = m.id;
        o["display_name"] = m.displayName;
        o["family"] = m.family;
        o["task"] = m.task;
        o["num_classes"] = m.numClasses;
        QJsonArray cls;
        for (const auto &c : m.classes) cls.append(c);
        o["classes"] = cls;
        o["input_size"] = QJsonArray{m.inputWidth, m.inputHeight};
        o["quant"] = m.quant;
        o["version"] = m.version.isEmpty() ? QDate::currentDate().toString("yyyy-MM-dd") : m.version;
        o["sha256"] = m.sha256;
        o["notes"] = m.notes;
        QFile out(metaFile);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
            out.close();
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    entries_[id] = m;
}

// ============================================================================
// 查询接口
// ============================================================================
QStringList ModelRegistry::ids() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    QStringList list = entries_.keys();
    std::sort(list.begin(), list.end());
    return list;
}

bool ModelRegistry::contains(const QString &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.contains(id);
}

ModelMeta ModelRegistry::meta(const QString &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.value(id);
}

QString ModelRegistry::modelPath(const QString &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.constFind(id);
    if (it == entries_.constEnd()) return QString();
    return it->dir + "/" + kModelFile;
}

QString ModelRegistry::labelPath(const QString &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.constFind(id);
    if (it == entries_.constEnd()) return QString();
    return it->dir + "/" + kLabelFile;
}

QString ModelRegistry::displayName(const QString &id) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.constFind(id);
    if (it == entries_.constEnd()) return id;
    return it->displayName.isEmpty() ? id : it->displayName;
}

QString ModelRegistry::findIdByModelFile(const QString &path) const
{
    if (path.trimmed().isEmpty()) return QString();
    const QString want = QFileInfo(path).absoluteFilePath();
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
        if (QFileInfo(it->dir + "/" + kModelFile).absoluteFilePath() == want)
            return it.key();
    }
    return QString();
}

// ============================================================================
// 导入外部模型（sha256 去重）
// ============================================================================
bool ModelRegistry::importModel(const QString &srcRknnPath, const QString &srcLabelPath,
                                const QString &id, QString &errorMsg)
{
    static const QRegularExpression reId("^[a-z0-9][a-z0-9_-]{0,62}$");
    if (!reId.match(id).hasMatch()) {
        errorMsg = QStringLiteral("模型 id 需为小写字母/数字/-/_，且以字母数字开头");
        return false;
    }
    const QFileInfo srcInfo(srcRknnPath);
    if (!srcInfo.exists() || !srcInfo.isFile()) {
        errorMsg = QStringLiteral("模型文件不存在: %1").arg(srcRknnPath);
        return false;
    }

    const QString sha = fileSha256(srcRknnPath);
    if (sha.isEmpty()) {
        errorMsg = QStringLiteral("无法读取模型文件: %1").arg(srcRknnPath);
        return false;
    }

    // 去重：库内已有同内容模型则不复制，直接返回既有 id
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = entries_.constBegin(); it != entries_.constEnd(); ++it) {
            if (it->sha256 == sha) {
                errorMsg = QStringLiteral("库内已有相同模型: %1").arg(it.key());
                return true;
            }
        }
    }

    QDir lib("model/library");
    if (!lib.exists() && !lib.mkpath(".")) {
        errorMsg = QStringLiteral("无法创建 model/library 目录");
        return false;
    }
    const QString dstDir = lib.absoluteFilePath(id);
    if (QDir(dstDir).exists()) {
        errorMsg = QStringLiteral("模型 id 已存在: %1").arg(id);
        return false;
    }
    if (!QDir().mkpath(dstDir)) {
        errorMsg = QStringLiteral("无法创建模型目录: %1").arg(dstDir);
        return false;
    }

    if (!QFile::copy(srcRknnPath, dstDir + "/" + kModelFile)) {
        errorMsg = QStringLiteral("复制模型文件失败");
        return false;
    }
    if (!srcLabelPath.isEmpty()) {
        if (!QFile::copy(srcLabelPath, dstDir + "/" + kLabelFile)) {
            errorMsg = QStringLiteral("复制标签文件失败");
            return false;
        }
    } else {
        QFile empty(dstDir + "/" + kLabelFile);
        empty.open(QIODevice::WriteOnly);
        empty.close();
    }

    loadEntryDir(dstDir);
    return true;
}

// ============================================================================
// 引用计数：有多少通道的模型列表用到了这个库模型
// ============================================================================
// V2 里配置不再存路径，通道直接按库目录名引用模型，所以计数是一次列表匹配，
// 不需要 findIdByModelFile 反查（那个函数仍保留给迁移与旧数据兼容用）。
// ============================================================================
int ModelRegistry::referenceCount(const QString &id) const
{
    return ConfigManager::instance().modelParamUsers(id).size();
}

// ============================================================================
// 从库中移除模型（移入 model/.trash/，不做物理删除）
// ============================================================================
bool ModelRegistry::deleteModel(const QString &id, QString &errorMsg)
{
    const ModelMeta m = meta(id);
    if (m.id.isEmpty()) {
        errorMsg = QStringLiteral("模型不存在: %1").arg(id);
        return false;
    }
    // 被通道引用 → 拒绝。报错里直接列出通道号（UI 显示 1~4，内部 0-based），
    // 省得现场再去设置页一个个翻。
    const QList<int> users = ConfigManager::instance().modelParamUsers(id);
    if (!users.isEmpty()) {
        QStringList chs;
        for (int ch : users) chs.append(QString::number(ch + 1));
        errorMsg = QStringLiteral("模型被通道 %1 引用，请先在对应通道的模型列表中解除引用")
                       .arg(chs.join(QStringLiteral("/")));
        return false;
    }
    // 只在参数表里挂着、没有任何通道引用，也拒绝：悬空定义会让人以为这模型还在用。
    if (ConfigManager::instance().modelParamKeys().contains(id)) {
        errorMsg = QStringLiteral("模型仍在模型参数表中定义，请先删除该条目");
        return false;
    }

    // 移入回收目录（library 之外，rescan 不会扫到），带时间戳防重名
    const QString trashDir = QStringLiteral("model/.trash");
    if (!QDir().mkpath(trashDir)) {
        errorMsg = QStringLiteral("无法创建回收目录: %1").arg(trashDir);
        return false;
    }
    const QString dst = trashDir + "/" + id + "-"
                        + QDateTime::currentDateTime().toString("yyyyMMdd-hhmmss");
    if (!QFile::rename(m.dir, QFileInfo(dst).absoluteFilePath())) {
        errorMsg = QStringLiteral("移动模型目录失败: %1").arg(m.dir);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.remove(id);
    }
    return true;
}

// ============================================================================
// 首次使用前自动扫描（decoder 构造早于主窗口，调用方无需手动 rescan）
// ============================================================================
void ModelRegistry::ensureScanned()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (scanned_)
            return;
        scanned_ = true;  // 先置位再扫描；扫描失败也不重试，等待显式 rescan()
    }
    rescan();
}

// ============================================================================
// 单通道级联槽位解析（decoder/UI 共用）
// ============================================================================
// 返回**只含可用槽位**的有序列表，下标即紧凑槽位号 = TaskConfig.result_id
// = 画框上的 M{n+1} = 报警限流键里的槽位段。
// 悬空引用（通道写了库里没有的模型）不占位：若占位，通道列表中间删掉一个模型
// 会让后面所有槽位的 result_id 集体挪号，现场看到的 M2/M3 标号跟着抖。
QList<ResolvedSlot> ModelRegistry::resolveChannelSlots(int ch)
{
    instance().ensureScanned();
    ConfigManager &cfg = ConfigManager::instance();

    QList<ResolvedSlot> resolved;
    const QStringList ids = cfg.channelModelIds(ch);
    for (const QString &libId : ids) {
        if (!instance().contains(libId)) {
            qWarning() << "[ModelRegistry] 通道" << ch << "引用的模型不在库中，该槽位跳过:"
                       << libId << QStringLiteral("（请在『模型管理』页导入 model/library/%1/model.rknn）").arg(libId);
            continue;
        }
        ResolvedSlot s;
        s.libId = libId;
        s.modelPath = instance().modelPath(libId);  // 绝对路径，给加载器用
        const VideoModelParam param = cfg.modelParam(libId);
        s.alarmClass = param.alarmClass;            // 空 = 这个模型只画框不报警
        // 标签优先级：参数表显式 label（库外标签文件的逃生口）> 库内 labels.txt
        s.labelPath = param.label.isEmpty() ? instance().labelPath(libId) : param.label;
        if (!QFile::exists(s.labelPath)) {
            qWarning() << "[ModelRegistry] 通道" << ch << "标签文件读不到，回落库内默认:" << s.labelPath;
            s.labelPath = instance().labelPath(libId);
        }
        // ===== 置信度回落链：每模型覆盖 → 全局 detect.conf_threshold → 0.25 =====
        // 走 resolveConfThreshold()，本函数与解码器兜底槽/模型测试/盘点共用同一条链。
        s.confThreshold = resolveConfThreshold(libId);
        s.nmsThreshold = resolveNmsThreshold();
        resolved.append(s);
    }
    return resolved;
}

// ============================================================================
// 阈值回落链（唯一实现处）
// ============================================================================
// 下限夹在这里而不是夹在 ConfigManager 的 getter 里，是因为配置文件也是用户输入：
// 手改一个 0 进去同样要拦住——量化模型把阈值打到 u8 网格上比较，阈值归 0 会让
// score_sum 预筛整体失效、候选爆到上限，接着是 O(N²) 的 NMS，推断线程卡死、
// 画面冻住，而且没有任何看门狗会把它拉起来（OBJ_NUMB_MAX_SIZE 只截输出，不救线程）。
float ModelRegistry::resolveConfThreshold(const QString &libId)
{
    ConfigManager &cfg = ConfigManager::instance();
    const float override_ = cfg.modelConfThreshold(libId);   // 负数 = 未配置
    const qreal base = override_ >= 0.f ? static_cast<qreal>(override_)
                                        : cfg.confThreshold();
    return static_cast<float>(qBound(0.05, base, 1.0));
}

float ModelRegistry::resolveNmsThreshold()
{
    // NMS 不设下限：调低是"更激进地合并重叠框"，方向上是变少而不是变多，
    // 没有置信度那种候选爆炸的失败模式，所以不给它编一个假护栏。
    return static_cast<float>(ConfigManager::instance().nmsThreshold());
}

// 路径反查不到库 id（未入库的临时模型）时，传空串给 resolveConfThreshold：
// modelConfThreshold("") 恒为负数 = "没有覆盖"，回落链自己就落到全局值上了，
// 所以这里不需要额外的"查不到"分支。
float ModelRegistry::resolveConfThresholdByPath(const QString &modelPath)
{
    return resolveConfThreshold(instance().findIdByModelFile(modelPath));
}
