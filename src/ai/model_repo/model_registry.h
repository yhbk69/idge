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
    // 该槽位的**生效**阈值：回落链（每模型 conf_threshold → detect.conf_threshold → 0.25）
    // 已在 resolveChannelSlots() 里走完并夹过下限，调用方拿到的就是最终值，不必再判缺键。
    // 类内默认值等于 detect() 的形参默认值，语义是"谁都没配 ⇒ 行为与接通阈值前逐字节相同"。
    float confThreshold = 0.25f;
    float nmsThreshold = 0.45f;
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

    // ========================================================================
    // 版本留存与回滚（2026-11-06 加）
    // ========================================================================
    // 归档根是 **data/model_versions/<id>/<version>/**，不是 model/ 下面：
    // `model/` 是 CMake 建进 git 工作树的软链，往那儿写等于把仓库搞脏（而且换
    // 构建目录就找不到归档了），data/ 才是运行期数据该待的地方。
    // 版本号取 meta.json 的 version（入库日期），同一天二次入库时目录名加 -2/-3。

    /// 归档根目录（相对当前工作区）
    static QString versionsRoot();

    /// 该 id 已归档的版本目录名，**新→旧**；没归档过返回空表
    QStringList archivedVersions(const QString &id) const;

    /**
     * @brief 覆盖式导入：id 已存在且内容不同 → 先归档现役那份，再写入新的
     * @return true = 已写入，或内容与现役一致（sha 相同，等于没改动，也返回 true）
     *
     * 与 importModel 的区别就一条：importModel 撞 id 直接拒绝（那是"新增"的语义），
     * 本函数是"换权重"：现场迭代模型时不改 id，改的是那一份 .rknn。
     */
    bool updateModel(const QString &id, const QString &srcRknnPath,
                     const QString &srcLabelPath, QString &errorMsg);

    /**
     * @brief 回滚到某个归档版本
     *
     * 现役那份同样先归档（所以回滚是可逆的：滚错了再滚回来，两个版本都在库里留着）。
     * 归档文件是**复制**回 model/library/<id>/，归档目录本身不动 —— 留着才能反复回滚。
     */
    bool rollbackModel(const QString &id, const QString &version, QString &errorMsg);

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

    /**
     * @brief 某模型在本进程的**生效**置信度阈值（回落链的唯一实现处）
     *
     * `video.models[<libId>].conf_threshold` → `detect.conf_threshold` → 0.25，
     * 结果夹到 [0.05, 1.0]。缺键与"配了 0"是两件事：前者跟随全局，后者会被夹回 0.05。
     *
     * 为什么单独暴露：四个表面要用同一个数——级联槽位解析、解码器的"库内默认模型"兜底槽、
     * 设置页「模型测试」、装备盘点。让它们各自读 config 就必然出现
     * "测试页框一堆、监控页一个不见"，而现场只会怀疑阈值没生效。
     */
    static float resolveConfThreshold(const QString &libId);

    /** @brief 生效 NMS 阈值（只有全局一档，见 ConfigManager::nmsThreshold 的说明） */
    static float resolveNmsThreshold();

    /**
     * @brief 按**模型文件路径**解析生效置信度（入参不是库 id）
     *
     * 给不经过通道槽位的旁路检测面用：设置页「模型测试」、装备盘点、摄像头预览对话框。
     * 这些表面只手里拿着一个 rknn 路径，反查不到库 id 就是"该模型没有覆盖"，
     * 回落链自动落到全局值——与级联槽位用的是同一个函数，不会出现两套数。
     */
    static float resolveConfThresholdByPath(const QString &modelPath);

private:
    ModelRegistry() {}

    void loadEntryDir(const QString &dirPath);  ///< 解析/合成单个模型目录并入库
    void ensureScanned();                       ///< 首次使用前自动 rescan 一次

    QHash<QString, ModelMeta> entries_;         ///< id → 元数据
    bool scanned_ = false;                      ///< 是否已完成首次扫描
    mutable std::mutex mutex_;                  ///< 保护 entries_（rescan 与查询并发）
};

#endif // MODELREGISTRY_H
