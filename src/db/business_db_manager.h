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
//   单个 sqlite3* 连接、无互斥保护。sqlite3 默认 serialized 模式，但本类未做任何
//   应用层同步，且开启 WAL/多连接并发需额外 PRAGMA（此处未设），故约定单线程使用
//   或由上层串行化调用；不要跨线程共享同一 BusinessDBManager 实例并发读写。
//
// ============================================================================

#include <string>
#include <vector>
#include <cstdint>
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
    
    bool open(const std::string& db_path);   // 打开库文件并幂等建表（IF NOT EXISTS）
    void close();
    
    // 任务操作
    int createTask(const std::string& name, const std::string& type, const std::string& folder_path);
    bool updateTask(int task_id, int registered_count, int cancelled_count, int is_cancelled);
    // 删除任务：因未开启 PRAGMA foreign_keys，CASCADE 不生效，
    // 故内部先手工删各子表数据、最后删 tasks 行（顺序敏感，见 .cpp）。
    bool deleteTask(int task_id);
    Task getTask(int task_id);               // 单行查询；无记录/未初始化返回 id=0 的空任务
    std::vector<Task> getAllTasks();         // 按 create_time 倒序
    std::vector<Task> getRegistrationTasks();  // 获取所有登记任务（type='registration'）
    
    // 人脸记录操作
    int insertFaceRecord(const FaceRecord& record);   // 返回新行 rowid，失败返回 -1
    bool deleteFaceRecordsByTask(int task_id);
    std::vector<FaceRecord> getFaceRecordsByTask(int task_id);
    std::vector<FaceRecord> getUniqueFacesByTask(int task_id);  // 获取去重后的人脸（is_duplicate=0）
    bool updateFaceRecord(int record_id, const FaceRecord& record);  // 仅更新路径/去重字段，不改 feature

    // 注销数据整体替换：事务内先删该 task 的旧 photos/matches，再批量插入（原子）
    bool replaceCancellationData(
        int task_id,
        const std::vector<CancellationPhotoRecord>& photos,
        const std::vector<CancellationMatchRecord>& matches);
    bool deleteCancellationDataByTask(int task_id);
    std::vector<CancellationPhotoRecord> getCancellationPhotosByTask(int task_id);
    std::vector<CancellationMatchRecord> getCancellationMatchesByTask(int task_id);

    std::vector<Task> getTasksByType(const std::string& type);
    // 装备数据整体替换：按 (task_id, phase) 分阶段替换。
    // 入参 detections[].photo_id 语义为"photos 数组下标"，本函数在事务内重映射为
    // 真实 equipment_photos.rowid 后再写入，保证二级外键指向本次新插入的父行。
    bool replaceEquipmentData(int task_id, int phase,
                              const std::vector<EquipmentPhotoRecord>& photos,
                              const std::vector<EquipmentDetectionRecord>& detections);
    std::vector<EquipmentPhotoRecord> getEquipmentPhotosByTask(int task_id, int phase);
    std::vector<EquipmentDetectionRecord> getEquipmentDetectionsByPhoto(int photo_id);
    bool deleteEquipmentDataByTask(int task_id);
private:
    sqlite3* db_;            // 唯一的原生 sqlite3 连接句柄（非线程安全共享）
    bool initialized_;       // open() 成功后置位；所有增删改查先检查它，未初始化即空返回
    
    bool createTables();
    std::vector<float> blobToFeature(const void* blob, int size);
    std::vector<uint8_t> featureToBlob(const std::vector<float>& feature);
};

#endif // BUSINESS_DB_MANAGER_H
