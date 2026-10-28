#ifndef MODELREGISTRY_H
#define MODELREGISTRY_H

/**
 * @file model_registry.h
 * @brief 模型库注册表 - 单例模式
 *
 * 管理 model/library/ 下的模型库，目录规范（详见 plan/model_management.md）：
 *   model/library/<id>/
 *     ├── model.rknn    固定文件名
 *     ├── labels.txt    该模型专属类别表（一行一类，顺序即 cls_id）
 *     └── meta.json     元数据（缺失时扫描自动生成）
 *
 * 职责：
 *   1. rescan()：扫描库目录，解析/补写 meta.json，计算 sha256
 *   2. id → 模型路径/标签路径/显示名/类别数 查询
 *   3. findIdByModelFile()：按模型文件反查库内 id（供旧配置兼容与排查）
 *   4. importModel()：外部 rknn+labels 入库，sha256 相同则去重返回既有 id
 *   5. resolveChannelSlots()：**单通道**级联槽位的"模型+标签+报警类别"单一事实源，
 *      config 里通道只写库 id 列表，路径与类别名一律从这里取；
 *      标签优先级 = video.models[<id>].label（逃生口）> 库内 labels.txt
 *   6. referenceCount()：被多少通道的模型列表引用（删除守卫与"被引用数"列）
 *
 * 线程约定：rescan/import 仅在启动与配置页操作时调用；
 * resolveChannelSlots/getters 只读哈希，重建期间通过内部互斥保护。
 * ⚠ 本类会取 ConfigManager 的锁（channelModelIds/modelParam/modelParamUsers），
 *   所以 ConfigManager 内部（load 的迁移流程）**不得**回调本类：
 *   std::mutex 不可重入，回头取锁就是启动死锁。
 */

#include <QString>
#include <QStringList>
#include <QList>
#include <QHash>
#include <mutex>

/**
 * @brief 某通道一个级联槽位解析后的模型描述（由 channelModelIds 逐项展开）
 *
 * 只描述**可用**槽位：库里找不到的 id 会被跳过并打告警，不占下标。
 */
struct ResolvedSlot
{
    QString modelPath;   ///< model.rknn 绝对路径（给加载器）
    QString labelPath;   ///< 该模型的 labels.txt（参数表覆盖 > 库内默认）
    QString libId;       ///< 模型库目录名，config 里就是这个键
    QString alarmClass;  ///< 该模型的报警类别（video.models[<id>].alarm_class），空=不报警
};

/**
 * @brief 库内单个模型的元数据
 */
struct ModelMeta
{
    QString id;            ///< 目录名即 id，如 "yolo11n-helmet"
    QString dir;           ///< 模型目录绝对路径
    QString displayName;   ///< 界面显示名
    QString family;        ///< 模型家族，如 "yolo11"
    QString task;          ///< 检测任务，如 "coco"/"ppe"/"helmet"
    int numClasses = 0;    ///< 类别数（labels.txt 行数）
    QStringList classes;   ///< 类别名列表
    int inputWidth = 640;  ///< 模型输入宽
    int inputHeight = 640; ///< 模型输入高
    QString quant;         ///< 量化方式 "int8"/"fp16" 等，未知为空
    QString version;       ///< 版本标记（默认用入库日期）
    QString sha256;        ///< model.rknn 内容哈希（完整性 + 去重）
    QString notes;         ///< 备注
};

class ModelRegistry
{
public:
    static ModelRegistry &instance();

    /**
     * @brief 重新扫描模型库目录
     * @param libraryDir 库目录，默认 "model/library"（相对当前工作目录）
     * @return 入库模型数；目录不存在时清空注册表并返回 0
     *
     * 对每个含 model.rknn 的子目录：meta.json 可读则解析，否则按
     * labels.txt/文件名合成并回写 meta.json（幂等，仅在内容变化时写）。
     */
    int rescan(const QString &libraryDir = QStringLiteral("model/library"));

    /// 库内全部模型 id（稳定排序）
    QStringList ids() const;

    /// id 是否存在于库中
    bool contains(const QString &id) const;

    /// id → 完整信息，不存在返回空 ModelMeta（id 字段为空即无效）
    ModelMeta meta(const QString &id) const;

    QString modelPath(const QString &id) const;    ///< id → model.rknn 绝对路径（加载器用）
    QString labelPath(const QString &id) const;    ///< id → labels.txt 绝对路径（加载器用）
    QString displayName(const QString &id) const;  ///< id → 显示名（无则回落 id）

    /**
     * @brief 按模型文件路径反查库内 id
     * @param path 任意写法的路径（相对/绝对），内部统一 absoluteFilePath 比较
     * @return 命中返回 id，未收编返回空串
     */
    QString findIdByModelFile(const QString &path) const;

    /**
     * @brief 将外部模型文件导入库目录
     * @param srcRknnPath  源 .rknn 文件
     * @param srcLabelPath 源标签文件（可为空，生成空 labels.txt）
     * @param id           目标 id（作为子目录名，需符合 [a-z0-9-_]+）
     * @param errorMsg     失败原因输出
     * @return true=导入成功或 sha256 去重命中既有模型（id 输出为既有 id）
     */
    bool importModel(const QString &srcRknnPath, const QString &srcLabelPath,
                     const QString &id, QString &errorMsg);

    /// id 被多少个通道的模型列表引用（删除守卫 + 表格"被引用数"列）
    int referenceCount(const QString &id) const;

    /**
     * @brief 从库中移除模型
     *
     * 被任一通道的模型列表引用时拒绝删除；否则把模型目录移入 model/.trash/
     * （带时间戳后缀，可人工恢复，不做物理删除），并同步注册表。
     *
     * @return true=已移除，false=被引用/目录移动失败（errorMsg 给原因）
     */
    bool deleteModel(const QString &id, QString &errorMsg);

    /**
     * @brief 解析某一通道的级联槽位（decoder/UI 的单一事实源）
     *
     * 输入通道号（0-based，与 video.channels 下标同口径），取其有序模型列表，
     * 逐项展开成模型路径 + 标签 + 该模型的报警类别。
     * 标签优先级：video.models[<id>].label（库外标签的逃生口）> 库内 labels.txt
     *
     * @return 只含可用槽位的有序列表，下标即紧凑槽位号（= result_id）；
     *         库里不存在的 id 被跳过并打告警，不占下标
     */
    static QList<ResolvedSlot> resolveChannelSlots(int ch);

private:
    ModelRegistry() {}

    void loadEntryDir(const QString &dirPath);  ///< 解析/合成单个模型目录并入库
    void ensureScanned();                       ///< 首次使用前自动 rescan 一次

    QHash<QString, ModelMeta> entries_;         ///< id → 元数据
    bool scanned_ = false;                      ///< 是否已完成首次扫描
    mutable std::mutex mutex_;                  ///< 保护 entries_（rescan 与查询并发）
};

#endif // MODELREGISTRY_H
