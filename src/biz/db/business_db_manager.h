#ifndef BUSINESS_DB_MANAGER_H
#define BUSINESS_DB_MANAGER_H

// ============================================================================
// business_db_manager.h - 人员点名 / 设备盘点业务数据库管理器
// ============================================================================
//
// 作用：
//   用原生 sqlite3 C API 管理"人员点名(人脸登记/注销)"与"设备盘点(装备登记/注销)"
//   两类业务的持久化。独立于检测报警所用的 idge.db（Qt SQL 那套），是本模块自持的
//   一个业务库文件，通过 open(db_path) 显式打开。
//
// 六张表及其关系（详见 .cpp 的 createTables）：
//   ┌ tasks                 业务任务主表（登记/注销都作为一种 task）
//   │   id (PK)
//   ├── face_records        人脸特征记录，       task_id  → tasks.id
//   ├── cancellation_photos 注销阶段照片，       task_id  → tasks.id
//   ├── cancellation_matches注销人脸匹配结果，   task_id  → tasks.id
//   ├── equipment_photos    装备盘点照片，       task_id  → tasks.id
//   └── equipment_detections装备检测结果框，     photo_id → equipment_photos.id
//   关系链：tasks 1—N (face_records / cancellation_* / equipment_photos)；
//           equipment_photos 1—N equipment_detections（唯一的二级子表）。
//
// 事务与外键：
//   - 建表语句写了 FOREIGN KEY ... ON DELETE CASCADE，但本文件从未执行
//     "PRAGMA foreign_keys = ON"（sqlite3 默认 OFF），所以外键约束实际不生效，
//     级联删除全部由 deleteTask()/deleteXxxByTask() 手工逐表 DELETE 兜底（见 .cpp）。
//   - replaceCancellationData / replaceEquipmentData 用 BEGIN IMMEDIATE ... COMMIT
//     把"先删后插"包成原子事务，中途失败统一 ROLLBACK，避免半更新状态。
//
// 线程安全：
//   单个 sqlite3* 连接由 mutex_ 做方法级互斥（recursive：deleteTask /
//   replaceCancellationData 等公开方法内部会调用其他公开方法）。
//   注意这只保证"单个方法原子"，跨方法的读-改-写序列仍需调用方自行串行化。
//
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
#include <mutex>
#include <sqlite3.h>

// tasks 表的一行：一个业务任务（登记或注销）
struct Task {
    int id;
    std::string name;
    std::string type;  // "registration", "cancellation" or "equipment_registration"
    std::string create_time;
    std::string folder_path;
    int registered_count;
    int cancelled_count;
    int is_cancelled;  // 0: 未注销, 1: 已注销
    int parent_task_id;  // 注销任务关联的登记任务ID
};

// face_records 表的一行：一张人脸的特征向量与来源照片
// feature 以 float blob 形式存于 feature_blob 列（featureToBlob/blobToFeature 互转）
struct FaceRecord {
    int id;
    int task_id;
    std::vector<float> feature;
    std::string original_photo_path;
    std::string processed_photo_path;
    std::string face_photo_path;
    int face_index;  // 该图中第几个人脸
    int is_duplicate;  // 0: 唯一, 1: 重复
    int similar_to_id;  // 重复时指向的原始人脸ID
};

// cancellation_photos 表的一行：注销阶段采集的照片（原图 + 处理图）
struct CancellationPhotoRecord {
    int id = 0;
    int task_id = 0;
    std::string original_photo_path;
    std::string processed_photo_path;
};

// cancellation_matches 表的一行：注销人脸与登记库的匹配结果
// similarity 为相似度分值（调用方阈值决定命中，本层不解释具体量纲/阈值）
struct CancellationMatchRecord {
    int id = 0;
    int task_id = 0;
    int registration_record_id = 0;
    std::string registration_image;
    std::string cancellation_image;
    float similarity = 0.0f;
    int status = 0;
};

// equipment_photos 表的一行：装备盘点阶段的一张照片
// phase: 0=登记(registration), 1=注销(cancellation)，用于区分同任务两阶段数据
struct EquipmentPhotoRecord {
    int id = 0;
    int task_id = 0;
    int phase = 0; // 0: registration, 1: cancellation
    std::string original_photo_path;
    std::string processed_photo_path;
};

// equipment_detections 表的一行：某张照片上的一个装备检测框
// 注意：入参填充时 photo_id 是"调用方 photos 数组的下标"（非真实外键），
//       replaceEquipmentData 会在事务内把它重映射为真实 equipment_photos.rowid（见 .cpp）。
struct EquipmentDetectionRecord {
    int id = 0;
    int photo_id = 0;
    int class_index = 0;
    std::string label;
    float confidence = 0.0f;
    int x1 = 0;
    int y1 = 0;
    int x2 = 0;
    int y2 = 0;
};

class BusinessDBManager {
public:
    BusinessDBManager();
    ~BusinessDBManager();   // 析构自动 close()，释放唯一 sqlite3* 连接
    
    /**
     * @brief 打开业务库文件并幂等建表（CREATE TABLE IF NOT EXISTS，六张表一起）
     * @param db_path 库文件路径；父目录**必须已存在**（sqlite 不会创建目录，
     *        路径写错只会得到一个笼统的 "unable to open database file"）
     * @return false：打开失败或建表失败（两种都不可用，调用方据返回值决定后续）
     *
     * 已 open 过再调会先 close 旧连接再开新的（不重复建表；initialized_ 随之重置）。
     * 与报警库 idge.db 完全无关，别把两个路径传反——两套 API 长得很像但句柄不通用。
     */
    bool open(const std::string& db_path);
    /// 关连接；未 open 时调用是安全的空操作。close 后所有查询退化成"空返回/-1"
    void close();
    
    // 任务操作
    /// 插入一行 tasks；name/type/folder_path 都不做非空与去重校验（业务层负责）
    /// 返回新行 rowid，未初始化或插入失败返回 -1
    int createTask(const std::string& name, const std::string& type, const std::string& folder_path);
    /// 整组覆盖三个计数/标记：传 0 就是"清零"，没有"跳过某列"的语义，
    /// 只想改一个字段也得先把另两个读回来（updateTask 不是 patch）
    bool updateTask(int task_id, int registered_count, int cancelled_count, int is_cancelled);
    // 删除任务：因未开启 PRAGMA foreign_keys，CASCADE 不生效，
    // 故内部先手工删各子表数据、最后删 tasks 行（顺序敏感，见 .cpp）。
    bool deleteTask(int task_id);
    Task getTask(int task_id);               // 单行查询；无记录/未初始化返回 id=0 的空任务
    std::vector<Task> getAllTasks();         // 按 create_time 倒序
    std::vector<Task> getRegistrationTasks();  // 获取所有登记任务（type='registration'）
    
    // 人脸记录操作
    /// 返回新行 rowid，失败返回 -1；record.feature 长度不做校验（512 维是上层约定），
    /// 变长即变长存 —— 换识别模型后必须清库重录，否则匹配阶段会按旧维度读越界
    int insertFaceRecord(const FaceRecord& record);   // 返回新行 rowid，失败返回 -1
    /// 只删 face_records 这一张表的行，不动 tasks，也不动磁盘上的人脸裁剪图
    bool deleteFaceRecordsByTask(int task_id);
    std::vector<FaceRecord> getFaceRecordsByTask(int task_id);
    std::vector<FaceRecord> getUniqueFacesByTask(int task_id);  // 获取去重后的人脸（is_duplicate=0）
    /// 仅更新三条路径与去重标记，**不改 feature_blob**（特征一旦写入就只读，见上）
    bool updateFaceRecord(int record_id, const FaceRecord& record);  // 仅更新路径/去重字段，不改 feature

    /**
     * @brief 注销数据整体替换：事务内先删该 task 的旧 photos/matches，再批量插入（原子）
     * @param photos/matches 允许为空 = "把这一类数据清空"（不是无操作）
     * @return false 时整批已 ROLLBACK，库里仍是调用前的样子（不会出现半更新态）
     *
     * 用 BEGIN IMMEDIATE 是为了一进事务就抢写锁：别的进程/连接在写时，冲突在 BEGIN
     * 就暴露，而不是删完旧行、插到一半才发现锁不到而必须回滚（那时已经删了）。
     * 副作用：matches 里的 id 字段被忽略（重新插入后 rowid 会变），调用方别指望保留。
     */
    bool replaceCancellationData(
        int task_id,
        const std::vector<CancellationPhotoRecord>& photos,
        const std::vector<CancellationMatchRecord>& matches);
    /// 只删 cancellation_photos + cancellation_matches 两张表（同一顺序约束：无 CASCADE）
    bool deleteCancellationDataByTask(int task_id);
    std::vector<CancellationPhotoRecord> getCancellationPhotosByTask(int task_id);
    /// 未匹配上的注销人脸（status=0/2）也存在这张表里由上层决定怎么显示，本层不筛
    std::vector<CancellationMatchRecord> getCancellationMatchesByTask(int task_id);

    /// type 是精确等值比较（不是前缀），取值只有 "registration" / "cancellation" /
    /// "equipment_registration" 三种（见 tasks.type 注释），拼错了返回空表且不报错
    std::vector<Task> getTasksByType(const std::string& type);
    // 装备数据整体替换：按 (task_id, phase) 分阶段替换。
    // 入参 detections[].photo_id 语义为"photos 数组下标"，本函数在事务内重映射为
    // 真实 equipment_photos.rowid 后再写入，保证二级外键指向本次新插入的父行。
    // 下标越界视为脏数据 → 整批 ROLLBACK（不是跳过那一条）：半份盘点结果比没有更难解释。
    bool replaceEquipmentData(int task_id, int phase,
                              const std::vector<EquipmentPhotoRecord>& photos,
                              const std::vector<EquipmentDetectionRecord>& detections);
    /// phase 必填（0=登记 / 1=注销），两阶段共用同一个 task_id，不带 phase 查不出来
    std::vector<EquipmentPhotoRecord> getEquipmentPhotosByTask(int task_id, int phase);
    std::vector<EquipmentDetectionRecord> getEquipmentDetectionsByPhoto(int photo_id);
    /// 两张表都删（先 detections 后 photos），且删的是**整个任务**的两个阶段，
    /// 没有按 phase 单删的接口——只撤消登记阶段请改用 replaceEquipmentData 传空列表
    bool deleteEquipmentDataByTask(int task_id);
private:
    sqlite3* db_;            // 唯一的原生 sqlite3 连接句柄（由 mutex_ 保护）
    bool initialized_;       // open() 成功后置位；所有增删改查先检查它，未初始化即空返回
    std::recursive_mutex mutex_;  // 方法级互斥（recursive 以允许公开方法互调）
    
    /// open() 内部调用：六张表 + 索引，全部 IF NOT EXISTS，重复执行无副作用
    bool createTables();
    /// blob → float 数组。按 size/4 决定长度，所以库里那行是裸字节、无头部无校验；
    /// 长度与期望维度不符（换了识别模型）时本层不报错，越界风险在调用方
    std::vector<float> blobToFeature(const void* blob, int size);
    /// float 数组 → 裸字节（memcpy 语义）：隐含同机 IEEE754 小端假设，跨端导入不可移植
    std::vector<uint8_t> featureToBlob(const std::vector<float>& feature);
};

#endif // BUSINESS_DB_MANAGER_H
