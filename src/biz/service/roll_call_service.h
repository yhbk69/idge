#ifndef ROLL_CALL_SERVICE_H
#define ROLL_CALL_SERVICE_H

/*
 * 人员点名服务（RollCallService）对外接口
 * 职责：串联“注册（点名登记）→ 注销比对”两条业务链——
 *   1) 注册：对批量照片做 SCRFD 人脸检测 + 识别模型特征提取（经外部可执行程序
 *      FaceRecognitionWrapper 调 RKNN/NPU），跨照片全局余弦相似度去重后写入 SQLite；
 *   2) 注销：将注销照片检测到的人脸与注册库做“一对一贪心匹配”，输出
 *      已注销/未注销/未登记 三类结果，确认后落库。
 * 内部自带线程池并行检测（照片级并行、去重与匹配串行），供 frm 层直接调用。
 */

#include <string>
#include <vector>
#include <memory>
#include "face_recognizer.h"
#include "business_db_manager.h"
#include <opencv2/opencv.hpp>
#include "thread_pool.h"  // 服务私有线程池：照片人脸检测在其后台线程并行执行

// 单张检测出的人脸的处理结果（贯穿“检测→去重→落库”三个阶段的中间载体）
struct ProcessedFace {
    cv::Rect rect;                // 人脸在原图中的包围框（x, y, w, h）
    std::vector<float> feature;   // 识别模型输出的特征向量（512 维，模型侧已 L2 归一化）
    bool is_duplicate;            // 全局去重阶段判定：是否与已见过的某张人脸重复
    int similar_to_index;         // 指向“唯一特征列表”的下标：重复=命中的旧人脸，唯一=自己新入表的下标
    int face_index;               // 该人脸在原始照片内的序号（检测器返回顺序）
    float score;                  // 人脸检测置信度（SCRFD 输出分数）
};

// 一张照片的完整处理结果（检测 + 去重标记 + 产物图片路径）
struct PhotoProcessResult {
    std::string original_path;            // 输入照片路径（注册阶段为原图路径，注销阶段为任务目录内副本）
    std::string processed_path;           // 绘制人脸框后的“后处理图”保存路径（保存失败时为空串）
    std::vector<ProcessedFace> faces;     // 该照片上检出的全部人脸（含去重标记）
    int unique_count;                     // 该照片贡献的“唯一（非重复）”人脸数
    std::vector<std::string> face_image_paths; // 本照片裁剪出的人脸小图路径（仅唯一人脸会裁剪保存）
};

// 一个注册任务的全量处理结果（照片结果聚合 + 人脸裁剪图清单）
struct TaskProcessResult {
    int task_id;
    std::string task_folder;                    // 任务文件夹（所有产物图片的落盘目录）
    std::vector<PhotoProcessResult> photos;     // 与输入照片一一对应
    int total_unique_count;                     // 全局去重后的唯一人脸总数（= 注册人数）
    std::vector<std::string> face_image_paths;  // 全部唯一人脸裁剪图，下标即“去重特征索引”
};

// 注销比对的一条匹配输出（一对一贪心匹配的结果行，供 UI 展示与落库）
struct CancellationMatch {
    int registration_record_id = 0;   // 命中的注册库记录 id；status=2（未登记）时为 0
    int cancellation_record_id = 0;   // 注销侧记录 id（当前流程未单独落库注销人脸，保留字段）
    std::string registration_image;   // 注册侧人脸图路径（优先裁剪小图）
    std::string cancellation_image;   // 注销侧人脸图路径（裁剪图为空时回退整照原图）
    float similarity = 0.0f;          // 归一化特征点积 = 余弦相似度；仅在匹配成功时有意义
    int status = 0;                   // 匹配状态：0=已登记但未注销，1=注销匹配成功，2=注销人脸未登记
};

// 注销阶段一张照片的产物路径对（用于回显“原图/后处理图”）
struct CancellationPhotoResult {
    std::string original_path;
    std::string processed_path;
};

// 注销比对的整体结果：照片产物清单 + 匹配明细
struct CancellationProcessResult {
    std::vector<CancellationPhotoResult> photos;
    std::vector<CancellationMatch> matches;
};

// 人员点名服务：注册（点名）、注销比对、任务生命周期管理三条流程的统一入口。
// 线程模型：本类自身不加锁，约定由调用方（UI 后台线程）串行驱动；
// 类内 thread_pool_ 仅用于把“逐照片检测”这一 CPU/NPU 密集步骤并行化。
class RollCallService {
public:
    RollCallService();
    ~RollCallService();
    
    // 初始化：加载外部识别程序与两个 RKNN 模型路径、打开 SQLite 注册库、创建存储目录
    //  @param exe_path      face_recognition 可执行程序（RKNN 推理宿主，子进程方式调用）
    //  @param det_model_path SCRFD 人脸检测 rknn 模型（工作区约定 model/face/detection.rknn）
    //  @param rec_model_path 人脸特征提取 rknn 模型（model/face/recognition.rknn，输出 512 维特征）
    //  @param db_path        SQLite 注册库文件（data/roll_call_data/roll_call.db）
    //  @param base_storage_path 任务文件夹根目录（data/roll_call_data/），每个任务一个子目录
    bool initialize(const std::string& exe_path,
                   const std::string& det_model_path,
                   const std::string& rec_model_path,
                   const std::string& db_path,
                   const std::string& base_storage_path);
    
    // 任务管理：任务 = 数据库行（tasks 表） + 磁盘文件夹（产物图片目录），二者同生共死
    int createRegistrationTask(const std::string& task_name);  // 先建文件夹再写库，库失败则回滚删文件夹
    bool deleteTask(int task_id);
    bool cancelTask(int task_id);                              // 仅置 is_cancelled 标记，不删数据
    Task getTaskInfo(int task_id);
    std::vector<Task> getAllTasks();

    //登记流程：processPhotos 只做检测+去重（不落库），saveTaskResult 才写库，
    //便于 UI 先展示“识别结果确认页”、用户确认后再持久化
    TaskProcessResult processPhotos(int task_id, const std::vector<std::string>& photo_paths);
    bool saveTaskResult(const TaskProcessResult& result);
    //注销流程：matchCancellation 只比对（不落库），confirmCancellation 用户确认后写库并标记任务注销
    CancellationProcessResult matchCancellation(
        int task_id, const std::vector<std::string>& photo_paths);
    bool confirmCancellation(
        int task_id, int cancelled_count,
        const CancellationProcessResult& result);
    
    
    // 以下访问器供 UI 层复用：getDatabase 让设备盘点服务共用同一 SQLite 连接
    BusinessDBManager* getDatabase() { return db_.get(); }
    const std::string& getStoragePath() const { return base_storage_path_; }
    const std::string& getDetectionModelPath() const { return detection_model_path_; }
    // 覆盖相似度阈值（默认 0.8，见构造函数）：越大越严格（少误合并），越小越宽松（多漏判重复）
    void setSimilarityThreshold(float threshold) { similarity_threshold_ = threshold; }
    
private:
    std::unique_ptr<FaceRecognitionWrapper> recognizer_;  // 外部人脸检测/识别程序封装（子进程 + JSON 解析）
    std::unique_ptr<BusinessDBManager> db_;               // SQLite：任务表 + 人脸注册表 + 注销记录表
    std::unique_ptr<ThreadPool> thread_pool_;             // 默认 4 工作线程，照片级并行检测
    std::string base_storage_path_;                       // 任务文件夹根目录
    std::string detection_model_path_;                    // 检测模型路径（对外展示/复用）
    float similarity_threshold_;                          // 余弦相似度判定阈值（去重与注销匹配共用）
    
    // 单张图片处理：仅“检测+特征提取”，不做去重——去重需要全局视角，留到合并阶段。
    // 设计为独立成员函数正是为了能作为线程池任务体并行调度（多张照片同时在检）
    PhotoProcessResult processSinglePhotoParallel(const std::string& photo_path,
                                                  const std::string& task_folder);
    
    // 合并各照片的检测结果做“全局去重”：按照片顺序逐人脸与已收集的唯一特征比对，
    // 唯一者入列表并裁剪保存人脸图，重复者仅记录 similar_to_index 指向的重复来源
    TaskProcessResult mergeAndDeduplicateResults(
        int task_id,
        const std::string& task_folder,
        std::vector<PhotoProcessResult>& photo_results);
    
    // 在原图副本上画框并保存“processed_”后处理图：绿色实线=唯一人脸，黄色虚线=重复人脸
    std::string drawAndSaveProcessedImage(const cv::Mat& image,
                                         const std::vector<ProcessedFace>& faces,
                                         const std::string& task_folder,
                                         const std::string& original_filename);
    
    // 按检测框裁剪人脸小图并保存（face_id 参与命名，重名会覆盖——注册流程内 id 唯一故安全）
    std::string saveDetectedFace(const cv::Mat& image,
                                 const cv::Rect& face_rect,
                                 const std::string& task_folder,
                                 int face_id,
                                 const std::string& name_prefix = "face");

    // 相似度查找（向量化版）：在已收集的唯一特征中找与 feature 余弦相似度最高、
    // 且超过 similarity_threshold_ 的下标；找不到返回 -1（视为新人脸）
    int findSimilarFaceVectorized(const std::vector<float>& feature,
                                   const std::vector<std::vector<float>>& unique_features);

    // L2 归一化：归一化后两向量内积 = 余弦相似度（|a|=|b|=1 时 a·b = cosθ），
    // 使“点积”无需再除以模长即可直接当作 [0,1] 附近的相似度使用
    void normalizeFeature(std::vector<float>& feature);

};

#endif // ROLL_CALL_SERVICE_H
