/* 人员点名服务实现（核心业务逻辑层）
人脸检测和特征提取：调用外部 RKNN 可执行程序（NPU 加速），按“每图一份 <图片路径>_faces.json”
    的约定回传检测结果，因此多张照片可同时并行检测而互不干扰。
去重算法：特征经 L2 归一化后，两向量内积即余弦相似度，超过 similarity_threshold_ 判为同一人。
图像处理：绘制人脸框（唯一=绿实线，重复=黄虚线）、裁剪人脸区域、保存图片产物。
任务管理：创建/删除任务，保存识别结果到数据库（先建文件夹后写库，失败回滚）。
注销匹配：将注销照片人脸与注册库人脸做“一对一贪心匹配”（逐行取最大 + used[] 占位），
    近似最优而非匈牙利算法的全局最优——取舍原因见 matchCancellation 内注释。
*/
#include "roll_call_service.h"
#include "../utils/task_manager.h"
#include "../utils/draw_utils.h"
#include <opencv2/opencv.hpp>
#include <iostream>
#include <algorithm>
#include <unistd.h>  // access()
#include <errno.h>   // errno
#include <cstring>   // strerror()
#include <sys/stat.h> // mkdir()
#include <sys/types.h>
#include <cstdlib>
#include <fstream>
#include <map>
#include <utility>
#include <QImage>
#include <cmath>
#include <chrono>

namespace {
// 读图并带兜底：优先 OpenCV 解码；板端部分镜像的 JPEG 后端会解码失败甚至崩溃，
// 此时退回调用 ImageMagick 的 convert 命令把任意受支持格式转成 PNG 再读。
// 临时文件名带 tag 和 pid：多张图/多进程并发转码时不会互相覆盖，读取后立即 unlink。
cv::Mat loadImageWithFallback(const std::string& path, const std::string& tag) {
    cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
    if (!image.empty()) return image;

    const std::string converted = "/tmp/idge_rollcall_" + tag + "_" +
                                  std::to_string(static_cast<long long>(getpid())) + ".png";
    const std::string command = "convert \"" + path + "\" \"" + converted + "\"";
    if (std::system(command.c_str()) == 0) {
        image = cv::imread(converted, cv::IMREAD_COLOR);
        unlink(converted.c_str());
    }
    return image;  // 两条路径都失败时返回空 Mat，由调用方判空跳过绘制/裁剪
}
}

RollCallService::RollCallService()
    : recognizer_(std::make_unique<FaceRecognitionWrapper>()),
      db_(std::make_unique<BusinessDBManager>()),
      thread_pool_(std::make_unique<ThreadPool>()),
      similarity_threshold_(0.8f) {
}// 阈值 0.8 为默认值：余弦相似度 >=0.8 即判为同一人（去重合并/注销命中），可用 setSimilarityThreshold 调整

// 析构依赖成员逆序销毁：thread_pool_ 最后声明、最先析构，其析构函数会 join 全部
// 工作线程，保证不会再有任务访问 recognizer_/db_ 之后它们才被释放——无悬垂访问
RollCallService::~RollCallService() = default;

bool RollCallService::initialize(const std::string& exe_path,
                                 const std::string& det_model_path,
                                 const std::string& rec_model_path,
                                 const std::string& db_path,
                                 const std::string& base_storage_path) {
    base_storage_path_ = base_storage_path;
    detection_model_path_ = det_model_path;
    
    // 创建存储目录（0755：属主可写、他人只读，任务文件夹将建在其下）
    mkdir(base_storage_path_.c_str(), 0755);
    
    // 配置识别器：外部可执行程序 + 检测/识别两个 rknn 模型，仅在 detectAndExtract 时读取
    recognizer_->setExecutablePath(exe_path);
    recognizer_->setDetectionModel(det_model_path);
    recognizer_->setRecognitionModel(rec_model_path);
    // 0.6=人脸检测置信度阈值（低于此分的检测框丢弃），0.4=NMS 去重的 IoU 阈值
    recognizer_->setThresholds(0.6f, 0.4f);  // 可配置
    
    // 打开数据库（SQLite 文件不存在时由 BusinessDBManager 建表初始化）
    if (!db_->open(db_path)) {
        std::cerr << "Failed to open database" << std::endl;
        return false;
    }
    
    std::cout << "RollCallService initialized successfully" << std::endl;
    return true;
}

// L2 归一化：feature /= ||feature||₂，使向量模长为 1。
// 为什么需要它：cos(a,b) = a·b / (|a||b|)，先把两边归一化，点积本身就等于余弦相似度，
// 后续所有比对只需一次内积、省去每次除模长；也统一了不同模型输出幅值差异。
// 1e-12 下限防除零：全零/退化特征保持原样（点积恒为 0，永远不会过阈值，天然不匹配）。
void RollCallService::normalizeFeature(std::vector<float>& feature) {
    float norm = 0.0f;
    for (float v : feature) {
        norm += v * v;
    }
    norm = std::sqrt(norm);
    
    if (norm > 1e-12f) {
        for (float& v : feature) {
            v /= norm;
        }
    }
}

// 向量化的相似度查找（替换原来逐特征手写循环的版本）：
// 在“已收集的唯一特征”里找与待查特征余弦相似度最高、且过阈值的那一条。
// 复杂度 O(R·d)（R=已注册数，d=特征维数 512），发生在注册去重阶段：
// 对每张新人脸跑一次，N 张人脸整体 O(N²·d)，去重规模（几十~几百脸）下可忽略不计。
// 注意：本函数每次都重新归一化全部在册特征（幂等但冗余）——为控制改动面暂未缓存，
// 归一化对已归一化向量无副作用，结果正确性不受影响。
int RollCallService::findSimilarFaceVectorized(
    const std::vector<float>& feature,
    const std::vector<std::vector<float>>& unique_features) {
    
    if (unique_features.empty()) {
        return -1;   // 库里还没有任何唯一人脸，第一张脸必然“唯一”
    }
    
    const int num_registered = unique_features.size();
    const int feature_dim = feature.size();
    
    // 归一化查询向量
    std::vector<float> query = feature;
    normalizeFeature(query);
    
    // 构建已注册特征矩阵（每行一个特征，归一化）
    std::vector<std::vector<float>> registered_matrix(num_registered);
    for (int i = 0; i < num_registered; ++i) {
        registered_matrix[i] = unique_features[i];
        normalizeFeature(registered_matrix[i]);
    }

    // 相似度即归一化向量的点积（此处按行手写内积，未引入 Eigen；规模小收益不显著）
    // 边界：若模型换维度导致在册特征比 query 短，此处按 feature_dim 读取会越界——
    // 注册库与识别模型必须同维度配套（见 README 注意事项）
    std::vector<float> similarities(num_registered);
    for (int i = 0; i < num_registered; ++i) {
        float s = 0.f;
        for (int j = 0; j < feature_dim; ++j) {
            s += registered_matrix[i][j] * query[j];
        }
        similarities[i] = s;
    }
    
    // 找最大值：把 max_similarity 初始化为阈值本身，是“一遍扫完即完成阈值过滤”的技巧——
    // 只有严格大于阈值的条目才可能更新 best_idx，返回值>=0 即等价“命中且过阈值”
    int best_idx = -1;
    float max_similarity = similarity_threshold_;
    
    for (int i = 0; i < num_registered; ++i) {
        if (similarities[i] > max_similarity) {
            max_similarity = similarities[i];
            best_idx = i;
        }
    }
    
    if (best_idx >= 0) {
        std::cout << "  Found similar face at index " << best_idx 
                  << " with similarity " << max_similarity << std::endl;
    }
    
    return best_idx;  // -1=未发现相似（当作新人脸）；>=0=命中 unique_features 下标
}

int RollCallService::createRegistrationTask(const std::string& task_name) {
    // 第一步：创建任务文件夹（重名时 TaskManager 自动追加 _1/_2 后缀保证唯一）
    std::string task_folder = TaskManager::createTaskFolder(base_storage_path_, task_name);
    if (task_folder.empty()) {
        std::cerr << "Failed to create task folder" << std::endl;
        return -1;
    }
    
    // 第二步：在数据库中创建任务记录；写库失败则回滚删除刚建的文件夹，
    // 避免留下“有目录无记录”的孤儿任务（文件夹与数据库行必须成对存在）
    int task_id = db_->createTask(task_name, "registration", task_folder);
    if (task_id < 0) {
        std::cerr << "Failed to create task in database" << std::endl;
        TaskManager::deleteTaskFolder(task_folder);
        return -1;
    }
    
    std::cout << "Created registration task: " << task_name << " (ID: " << task_id << ")" << std::endl;
    return task_id;
}

// 并行版本的单张图片处理（只做检测，不做去重）——线程池任务体。
// 并行安全性依据：
//   1) detectAndExtract 以“子进程执行外部程序 + 结果写 <图片路径>_faces.json”方式工作，
//      JSON 路径按图片名区分，不同照片并发处理不会互相覆盖；
//   2) recognizer_ 的路径/阈值成员在多任务间只读，无共享可变状态；
//   3) 返回的 PhotoProcessResult 按值返回，经 std::future 传回主线程，无数据竞争。
// 注意：同一 photo_paths 列表内部不得出现重复路径（同名文件并发写同一 JSON 会竞争）。
PhotoProcessResult RollCallService::processSinglePhotoParallel(
    const std::string& photo_path,
    const std::string& task_folder) {
    
    PhotoProcessResult result;
    result.original_path = photo_path;
    result.unique_count = 0;
    
    std::cerr << "[rollcall] Processing photo (parallel): " << photo_path << std::endl;
    
    // 调用exe检测人脸（SCRFD 检测 + 逐脸特征提取，一次子进程调用完成两阶段），计时用于性能观测
    auto start = std::chrono::steady_clock::now();
    FaceDetectionResult det_result = recognizer_->detectAndExtract(photo_path);
    auto end = std::chrono::steady_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Detected " << det_result.num_faces << " faces in " 
              << duration << "ms" << std::endl;
    
    // 暂时保存所有检测到的人脸（不做去重，稍后统一处理）：
    // 去重必须看到全部照片才能保证“全局唯一”，并行阶段仅收集原始证据
    for (const auto& detected_face : det_result.faces) {
        ProcessedFace face;
        face.rect = detected_face.bbox;
        face.face_index = detected_face.face_id;
        face.feature = detected_face.feature;
        face.score = detected_face.score;
        face.is_duplicate = false;  // 暂时标记，后续去重时更新
        face.similar_to_index = -1;  // -1=尚未判定归属
        
        result.faces.push_back(face);
    }
    
    return result;
}

// 合并所有图片的结果并去重（串行，跑在调用线程上）。
// 算法：维护 unique_features（已判定唯一的人脸特征列表，按发现顺序）。
// 每张照片的每张脸与整个列表比对一次（findSimilarFaceVectorized，O(R·d)）：
//   命中阈值 -> 标记重复，similar_to_index 指向被重复的旧脸（供画黄框/落库指向）；
//   未命中   -> 追加进列表，裁剪保存人脸小图，similar_to_index 即自己新入表的下标。
// 顺序敏感性：按照片序、照片内脸序贪心处理，“先出现的脸”成为保留者；
// 若同一人先以低质量图入册，后续高质量图会被判重复——注册场景通常可接受。
// 同一照片列表的输入顺序变化会导致去重归属不同，但唯一人数不变。
TaskProcessResult RollCallService::mergeAndDeduplicateResults(
    int task_id,
    const std::string& task_folder,
    std::vector<PhotoProcessResult>& photo_results) {
    
    TaskProcessResult final_result;
    final_result.task_id = task_id;
    final_result.task_folder = task_folder;
    final_result.total_unique_count = 0;
    
    std::vector<std::vector<float>> unique_features;
    
    // 遍历所有图片的所有人脸，做全局去重
    for (auto& photo_result : photo_results) {
        // 后处理（裁剪/画框）需要原图像素，此时才重新读图（检测在子进程里已完成，
        // 主进程不持有 Mat，避免并行阶段跨线程传大图）
        cv::Mat image = loadImageWithFallback(photo_result.original_path, "merge");
        
        for (auto& face : photo_result.faces) {
            // 使用矩阵运算查找相似人脸
            int similar_index = findSimilarFaceVectorized(face.feature, unique_features);
            
            if (similar_index >= 0) {
                // 重复人脸
                face.is_duplicate = true;
                face.similar_to_index = similar_index;
                std::cout << "  Face in " << photo_result.original_path 
                          << " is duplicate of index " << similar_index << std::endl;
            } else {
                // 唯一人脸
                face.is_duplicate = false;
                face.similar_to_index = unique_features.size();
                unique_features.push_back(face.feature);
                
                // 保存人脸裁剪图（为 UI 逐脸展示和注销阶段回显提供小图）
                if (!image.empty()) {
                    std::string face_path = saveDetectedFace(
                        image, face.rect, task_folder, unique_features.size() - 1);
                    photo_result.face_image_paths.push_back(face_path);
                    final_result.face_image_paths.push_back(face_path);
                }
                
                photo_result.unique_count++;
                std::cout << "  Face in " << photo_result.original_path 
                          << " is unique (index " << face.similar_to_index << ")" << std::endl;
            }
        }
        
        // 绘制后处理图（带人脸框）：绿实线=唯一、黄虚线=重复；文件名取原图 basename
        if (!image.empty()) {
            photo_result.processed_path = drawAndSaveProcessedImage(
                image, photo_result.faces, task_folder,
                photo_result.original_path.substr(photo_result.original_path.find_last_of("/\\") + 1));
        }
        
        final_result.photos.push_back(photo_result);
    }
    
    final_result.total_unique_count = unique_features.size();
    
    std::cout << "Deduplication complete: " << final_result.total_unique_count 
              << " unique faces from " << photo_results.size() << " photos" << std::endl;
    
    return final_result;
}

// 注册流程入口：processPhotos 采用“并行检测 + 串行去重”两段式流水线。
// 分工依据：检测要跑 NPU/子进程、单张耗时数百 ms~秒级，是绝对瓶颈，值得并行；
// 去重仅涉及内存内积运算，N 脸整体 O(N²·d) 但常数极小，串行反而避免加锁。
TaskProcessResult RollCallService::processPhotos(
    int task_id, const std::vector<std::string>& photo_paths) {
    
    Task task = db_->getTask(task_id);
    if (task.id == 0) {   // id==0 是 BusinessDBManager 约定的“查无此任务”哨兵值
        std::cerr << "Task not found: " << task_id << std::endl;
        return TaskProcessResult{};
    }
    
    const size_t num_photos = photo_paths.size();
    std::cout << "Starting parallel processing of " << num_photos << " photos..." << std::endl;
    
    auto start = std::chrono::steady_clock::now();
    
    // 1. 并行检测所有图片的人脸：一次性全部 submit（线程池队列无界，不会阻塞提交），
    //    实际并发度受池大小限制（默认 4，即最多 4 张同时检测）
    std::vector<std::future<PhotoProcessResult>> futures;
    futures.reserve(num_photos);
    
    for (const auto& photo_path : photo_paths) {
        futures.push_back(
            thread_pool_->submit(
                &RollCallService::processSinglePhotoParallel,
                this,
                photo_path,
                task.folder_path
            )
        );
    }
    
    // 2. 等待所有检测完成：future.get() 按提交顺序阻塞收结果；
    //    子任务抛异常时 get() 重放异常，这里捕获后跳过该照片（部分成功策略：
    //    坏图不阻断整批点名，代价是该照片的人脸不计入人数）
    std::vector<PhotoProcessResult> photo_results;
    photo_results.reserve(num_photos);
    
    for (auto& future : futures) {
        try {
            photo_results.push_back(future.get());
        } catch (const std::exception& e) {
            std::cerr << "Error processing photo: " << e.what() << std::endl;
        }
    }
    
    auto detect_end = std::chrono::steady_clock::now();
    auto detect_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        detect_end - start).count();
    
    std::cout << "Detection phase completed in " << detect_duration << "ms" << std::endl;
    
    // 3. 串行去重（去重速度很快，不需要并行）
    TaskProcessResult result = mergeAndDeduplicateResults(
        task_id, task.folder_path, photo_results);
    
    auto end = std::chrono::steady_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    std::cout << "Total processing time: " << total_duration << "ms "
              << "(detection: " << detect_duration << "ms, "
              << "dedup: " << (total_duration - detect_duration) << "ms)" << std::endl;
    
    return result;  // 只返回结果不落库，由调用方确认后调 saveTaskResult
}


// 在原图副本上绘制全部人脸框并保存“processed_”图。
// 线宽 4 是在 1080p~4K 工地照片上肉眼可辨的折中值；先 clone 是为了不污染调用方
// 还要继续用于裁剪的原始 image（去重阶段裁剪在前、画框在后，共用同一份像素）。
// 落盘走 QImage::save 而非 cv::imwrite：规避部分板端镜像 OpenCV JPEG/PNG 编码后端缺失/崩溃问题。
std::string RollCallService::drawAndSaveProcessedImage(const cv::Mat& image,
                                                       const std::vector<ProcessedFace>& faces,
                                                       const std::string& task_folder,
                                                       const std::string& original_filename) {
    cv::Mat result_image = image.clone();
    
    // 绘制人脸框
    for (const auto& face : faces) {
        if (face.is_duplicate) {
            // 黄色虚线框（重复）
            DrawUtils::drawDashedYellowBox(result_image, face.rect, 4);
        } else {
            // 绿色实线框（唯一）
            DrawUtils::drawSolidGreenBox(result_image, face.rect, 4);
        }
    }
    
    // 生成文件名（时间戳+随机数防重名，多次跑同一任务不覆盖历史产物）
    std::string filename = TaskManager::generateUniqueFilename("processed_" + original_filename, "png");
    std::string save_path = task_folder + "/" + filename;
    
    // 保存图像。QImage 视图仅是 result_image 内存的浅封装，步长按 Mat::step 传入
    // 以正确处理行填充；copy() 断开对 cv::Mat 内存的引用后再落盘，避免悬垂读
    QImage qimg(result_image.data, result_image.cols, result_image.rows, (int)result_image.step, QImage::Format_BGR888);
    if (qimg.copy().save(QString::fromStdString(save_path), "PNG")) {
        std::cout << "  Saved processed image: " << save_path << std::endl;
        return save_path;
    }
    
    std::cerr << "  Failed to save processed image" << std::endl;
    return "";  // 保存失败不致命：调用方以空串区分，仅少一张回显图
}

// 裁剪并保存单张人脸小图（供 UI 逐脸展示、注销结果回显）。
// 裁剪用 DrawUtils::cropFaceRegion：与检测框不同，保存的小图会外扩一点留头发/耳朵，
// 且内部已与图像边界求交，防止检测框越界导致 cv::Mat ROI 断言崩溃
std::string RollCallService::saveDetectedFace(const cv::Mat& image,
                                              const cv::Rect& face_rect,
                                              const std::string& task_folder,
                                              int face_id,
                                              const std::string& name_prefix) {
    std::cerr << "[rollcall] saveDetectedFace begin image=" << image.cols << "x" << image.rows
              << " rect=" << face_rect.x << "," << face_rect.y << " "
              << face_rect.width << "x" << face_rect.height << std::endl;
    // 裁剪人脸区域
    cv::Mat face_image = DrawUtils::cropFaceRegion(image, face_rect);
    std::cerr << "[rollcall] crop returned empty=" << face_image.empty() << std::endl;
    if (face_image.empty()) {
        return "";   // 框完全在图外（异常检测框）：放弃保存，调用方容忍空路径
    }
    
    // 生成文件名：face_<全局唯一序号>.png，序号即去重特征索引，与 similar_to_index 对应
    std::string filename = name_prefix + "_" + std::to_string(face_id) + ".png";
    std::string save_path = task_folder + "/" + filename;
    
    // 保存人脸图
    std::cerr << "[rollcall] writing face path=" << save_path << std::endl;
    // Some board images have a crashing OpenCV JPEG encoder. Keep the
    // registration flow alive; the processed/original images are still saved.
    // （板端镜像的 OpenCV 编码器可能崩溃/抛异常——这里连异常一并捕获，
    // 单张小图保存失败绝不允许中断整个注册流程）
    std::vector<unsigned char> encoded;
    bool saved = false;
    try {
        QImage qimg(face_image.data, face_image.cols, face_image.rows, (int)face_image.step, QImage::Format_BGR888);
        saved = qimg.copy().save(QString::fromStdString(save_path), "PNG");
    } catch (const std::exception& e) {
        std::cerr << "  Face encode/write exception: " << e.what() << std::endl;
        saved = false;
    }
    if (saved) {
        return save_path;
    }
    
    return "";
}

// 注册结果落库（用户点“确认”后调用）。可重复执行：先删本任务旧记录再整体重写，
// 保证“重新识别”后库里与最新一次处理结果一致（删除+重插的幂等覆盖策略）。
bool RollCallService::saveTaskResult(const TaskProcessResult& result) {
    std::cerr << "[rollcall] saveTaskResult begin task=" << result.task_id << " photos=" << result.photos.size() << " image_paths=" << result.face_image_paths.size() << std::endl;
    // 删除任务的旧记录
    db_->deleteFaceRecordsByTask(result.task_id);
    
    // 保存人脸记录到数据库
    int global_face_id = 0;
    std::map<int, int> unique_index_to_record_id;  // 映射unique索引到数据库record_id
    
    for (const auto& photo : result.photos) {
        std::cerr << "[rollcall] saving photo faces=" << photo.faces.size() << std::endl;
        for (const auto& face : photo.faces) {
            std::cerr << "[rollcall] inserting face id=" << face.face_index << " duplicate=" << face.is_duplicate << " feat=" << face.feature.size() << std::endl;
            FaceRecord record;
            record.task_id = result.task_id;
            record.feature = face.feature;   // 原始特征入库（注销比对时再归一化）
            record.original_photo_path = photo.original_path;
            record.processed_photo_path = photo.processed_path;
            record.face_index = face.face_index;
            record.is_duplicate = face.is_duplicate ? 1 : 0;
            
            if (face.is_duplicate) {
                // 重复人脸，指向原始人脸的record_id
                // 依赖“照片按序插入且唯一脸先于其重复脸入库”，故映射表此时必已含该索引；
                // 若映射缺失 operator[] 会补 0（similar_to_id=0 即“无指向”），不崩溃但溯源丢失
                record.similar_to_id = unique_index_to_record_id[face.similar_to_index];
                record.face_photo_path = "";  // 重复脸不单独存裁剪图（图在原始脸那条记录上）
            } else {
                // 唯一人脸：face_image_paths 下标与 similar_to_index（去重序号）一一对应；
                // 裁剪保存失败时该表可能短于索引值，越界判断后留空路径兜底
                record.similar_to_id = 0;
                if (face.similar_to_index < result.face_image_paths.size()) {
                    record.face_photo_path = result.face_image_paths[face.similar_to_index];
                }
            }
            
            int record_id = db_->insertFaceRecord(record);
            if (record_id > 0 && !face.is_duplicate) {
                unique_index_to_record_id[face.similar_to_index] = record_id;
            }
            
            global_face_id++;
        }
    }
    
    // 更新任务统计信息：registered=唯一人数；cancelled_count=0、is_cancelled=0 重置注销状态
    bool success = db_->updateTask(result.task_id, result.total_unique_count, 0, 0);
    
    if (success) {
        std::cout << "Task result saved successfully" << std::endl;
    } else {
        std::cerr << "Failed to save task result" << std::endl;
    }
    
    return success;
}

bool RollCallService::deleteTask(int task_id) {
    // 获取任务信息（拿 folder_path，同时校验任务存在：id==0 即不存在）
    Task task = db_->getTask(task_id);
    if (task.id == 0) {
        return false;
    }
    
    // 删除文件夹（先删产物再删库行；若删库失败留下“无记录孤儿目录”，可手工清理，
    // 反向顺序则会留下指向已消失目录的记录，UI 回显全空，更糟）
    if (!task.folder_path.empty()) {
        TaskManager::deleteTaskFolder(task.folder_path);
    }
    
    // 删除数据库记录（级联删除该任务的人脸/注销记录由 BusinessDBManager 负责）
    return db_->deleteTask(task_id);
}

// 软删除“任务级注销”标记：置 is_cancelled=1，保留全部登记数据以便追溯；
// 已注销任务不重复处理（幂等保护）
bool RollCallService::cancelTask(int task_id) {
    Task task = db_->getTask(task_id);
    if (task.id == 0 || task.is_cancelled) return false;
    return db_->updateTask(task_id, task.registered_count,
                           task.cancelled_count, 1);
}

// 注销比对入口：与注册流程同构的“并行检测 + 串行匹配”，差别在于匹配对象是
// 注册库（SQLite 中本任务的全部唯一人脸），且要求“一对一”约束：
// 一张注册脸最多被一张注销照命中，一张注销脸也最多认领一条注册记录。
// 整体流程（对应下方编号 1~7）：
//   1 取注册库 → 2 并行检测注销照片 → 3 汇总注销人脸(裁剪/画框产物) →
//   4 双侧特征归一化 → 5 相似度定义 → 6 贪心一对一匹配 → 7 收集未认领的注销脸
CancellationProcessResult RollCallService::matchCancellation(
    int task_id, const std::vector<std::string>& photo_paths) {
    
    CancellationProcessResult result;
    const Task task = db_->getTask(task_id);
    if (task.id == 0) return result;  // 任务不存在：返回空结果（photos/matches 皆空）

    // 1. 获取登记人脸：只取 is_duplicate=0 的唯一记录（重复脸的特征与其相同，
    //    再参与匹配只会产生冗余命中）
    auto registered = db_->getUniqueFacesByTask(task_id);
    if (registered.empty()) {
        std::cerr << "No registered faces for task " << task_id << std::endl;
        return result;   // 空注册库：无从匹配，返回空
    }

    const int num_registered = registered.size();
    const int feature_dim = 512;   // 识别模型约定的特征维数（文档性常量：真正的维度
                                   // 保护在下方 lambda 里按 min(a,b) 截断，防止两侧
                                   // 维度不一致时越界——模型与注册库必须同模型配套）
    
    std::cout << "Loaded " << num_registered << " registered faces" << std::endl;
    std::cout << "Starting parallel detection of " << photo_paths.size() 
              << " cancellation photos..." << std::endl;

    auto start = std::chrono::steady_clock::now();

    // 2. 并行检测注销照片：复用注册流程的线程池任务体 processSinglePhotoParallel
    //    （检测逻辑完全相同；注销照片不走去重，所以此处只取“检出了哪些脸”）
    std::vector<std::future<PhotoProcessResult>> futures;
    futures.reserve(photo_paths.size());
    
    for (const auto& path : photo_paths) {
        futures.push_back(
            thread_pool_->submit(
                &RollCallService::processSinglePhotoParallel,
                this,
                path,
                task.folder_path
            )
        );
    }
    
    // 等待检测完成：同 processPhotos 的容错策略——单照异常仅告警跳过，不中断整批
    std::vector<PhotoProcessResult> photo_results;
    for (auto& future : futures) {
        try {
            photo_results.push_back(future.get());
        } catch (const std::exception& e) {
            std::cerr << "Error detecting cancellation photo: " << e.what() << std::endl;
        }
    }
    
    auto detect_end = std::chrono::steady_clock::now();
    auto detect_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        detect_end - start).count();
    
    std::cout << "Cancellation detection completed in " << detect_duration << "ms" << std::endl;

    // 3. 汇总所有检测到的注销人脸：把“按照片组织”的检测结果摊平成一维 cancelled 列表，
    //    下标 cancel_idx 即匹配阶段的“列号”。同时产出 UI 回显素材：
    //    每张脸裁小图（cancel_face_*.png）、每张照片画框存后处理图（cancel_processed_*.png）
    std::vector<FaceRecord> cancelled;
    int cancellation_face_index = 0;   // 跨照片连续编号，保证裁剪图文件名不冲突
    
    for (auto& photo_result : photo_results) {
        cv::Mat image = loadImageWithFallback(photo_result.original_path, "cancel");
        std::vector<ProcessedFace> processed_faces;   // 仅用于绘制（注销侧无重复概念，全按绿框画）
        
        for (const auto& face : photo_result.faces) {
            FaceRecord r;
            r.feature = face.feature;
            r.original_photo_path = photo_result.original_path;
            
            // 保存裁剪的人脸图（注销匹配结果表里 cancel_face 优先展示小图）
            if (!task.folder_path.empty() && !image.empty()) {
                r.face_photo_path = saveDetectedFace(
                    image, face.rect, task.folder_path,
                    cancellation_face_index, "cancel_face");
            }
            ++cancellation_face_index;
            cancelled.push_back(std::move(r));
            
            // 用于绘制后处理图
            ProcessedFace processed_face;
            processed_face.rect = face.rect;
            processed_face.feature = face.feature;
            processed_face.is_duplicate = false;
            processed_face.similar_to_index = -1;
            processed_face.face_index = face.face_index;
            processed_face.score = face.score;
            processed_faces.push_back(std::move(processed_face));
        }
        
        // 绘制后处理图：文件名取原图 basename（find_last_of 未命中时 npos+1==0，等价整串）
        CancellationPhotoResult cancel_photo;
        cancel_photo.original_path = photo_result.original_path;
        if (!image.empty() && !task.folder_path.empty()) {
            const size_t slash = photo_result.original_path.find_last_of("/\\");
            const std::string filename = slash == std::string::npos
                ? photo_result.original_path 
                : photo_result.original_path.substr(slash + 1);
            cancel_photo.processed_path = drawAndSaveProcessedImage(
                image, processed_faces, task.folder_path, "cancel_" + filename);
        }
        result.photos.push_back(std::move(cancel_photo));
    }

    const int num_cancelled = cancelled.size();
    std::cout << "Detected " << num_cancelled << " cancellation faces" << std::endl;

    if (cancelled.empty()) {
        return result;   // 注销照里一张脸都没检出：所有注册记录将保持原样，直接返回
    }

    // 4. 构建特征矩阵（归一化）：注册侧 R 行、注销侧 C 行，每行 d 维单位向量。
    //    预归一化的目的：匹配阶段每对 (i,j) 的相似度只需一次内积即等于余弦相似度，
    //    避免在 O(R·C) 的重循环内反复计算模长（归一化从 O(R·C·d) 摊薄为 O((R+C)·d)）
    std::vector<std::vector<float>> registered_matrix(num_registered);
    for (int i = 0; i < num_registered; ++i) {
        registered_matrix[i] = registered[i].feature;
        normalizeFeature(registered_matrix[i]);
    }

    std::vector<std::vector<float>> cancelled_matrix(num_cancelled);
    for (int i = 0; i < num_cancelled; ++i) {
        cancelled_matrix[i] = cancelled[i].feature;
        normalizeFeature(cancelled_matrix[i]);
    }

    // 5. 相似度即归一化向量的点积（按需计算配对）：
    //    k 循环取 a、b 长度的较小者截断，两侧维度不一致（换过识别模型等异常场景）时
    //    退化为“前缀维内积”而非越界崩溃。未预生成完整 R×C 相似度矩阵：
    //    贪心本身就要逐行扫描，按需计算省下 R·C 浮点的存储，总计算量同为 O(R·C·d)
    auto similarity = [](const std::vector<float>& a, const std::vector<float>& b) {
        float s = 0.f;
        for (size_t k = 0; k < a.size() && k < b.size(); ++k) {
            s += a[k] * b[k];
        }
        return s;
    };
    
    auto match_start = std::chrono::steady_clock::now();
    std::cout << "Computing similarity: " << num_registered << "x" 
              << num_cancelled << std::endl;

    // 6. 贪心一对一匹配（近似算法，取舍说明）：
    //    按注册表顺序逐行处理：在第 reg_idx 行的所有“未被占用”的列中找相似度最大值，
    //    超过 similarity_threshold_ 才认领，并置 used[col]=true 排除该列——行、列均至多一度。
    //    为什么用贪心而不用匈牙利/KM 求全局最优：
    //      a) 复杂度：匈牙利为 O(n³)（n=max(R,C)），贪心仅 O(R·C·d) 扫一遍相似度，
    //         工地点名场景 R、C 通常几十~几百，两者都可接受，但贪心实现简单无数值库依赖；
    //      b) 语义：注册库顺序=登记先后（人脸序号），先到先认领符合“为每位在册人员找
    //         最像的注销者”的业务直觉；歧义（一人多脸争抢）在本场景罕见，因为两侧都是
    //         同一任务的去重后人脸，同人多脸已被去重压掉；
    //      c) 代价：贪心非全局最优——若 reg[0] 抢先认领了与 reg[1] 更像的注销脸，
    //         reg[1] 只能退而求其次甚至漏配（结果对注册表顺序敏感）。可接受：
    //         匹配结果本就仅供 UI 展示、由人工确认后 confirmCancellation 落库。
    //    未过阈值的注册者记 status=0（未注销）：兜底策略是“宁可漏注销、不可错注销”，
    //    错误合并会把仍在场的人标记为离场，业务上不可逆；漏配只是留待人工复核。
    std::vector<bool> used(num_cancelled, false);   // 列占用标记：实现“一张注销脸只配一人”
    
    for (int reg_idx = 0; reg_idx < num_registered; ++reg_idx) {
        const auto& reg = registered[reg_idx];
        
        CancellationMatch m;
        m.registration_record_id = reg.id;
        m.registration_image = reg.face_photo_path;
        
        // best_score 以阈值为初值：与 findSimilarFaceVectorized 同一技巧，
        // 单遍扫描同时完成“取行最大”与“过阈值过滤”
        int best_cancel_idx = -1;
        float best_score = similarity_threshold_;
        
        // 在第 reg_idx 行找最大值（未使用的列）
        for (int cancel_idx = 0; cancel_idx < num_cancelled; ++cancel_idx) {
            if (used[cancel_idx]) continue;
            
            float score = similarity(registered_matrix[reg_idx], cancelled_matrix[cancel_idx]);
            if (score > best_score) {
                best_score = score;
                best_cancel_idx = cancel_idx;
            }
        }
        
        if (best_cancel_idx >= 0) {
            used[best_cancel_idx] = true;   // 列一旦被认领即从后续所有行的候选中剔除
            m.status = 1;  // 注销匹配
            m.similarity = best_score;
            m.cancellation_image = cancelled[best_cancel_idx].face_photo_path;
            if (m.cancellation_image.empty()) {
                // 兜底：裁剪小图保存失败（磁盘满/解码崩溃）时退回整照原图路径，保证 UI 可显示
                m.cancellation_image = cancelled[best_cancel_idx].original_photo_path;
            }
            
            std::cout << "  Matched reg[" << reg_idx << "] with cancel[" 
                      << best_cancel_idx << "] similarity=" << best_score << std::endl;
        } else {
            m.status = 0;  // 未注销：全列最大都不过阈值，判该在册人员本次未出现
            m.similarity = 0.0f;   // 未命中不保留“差一点”的分数，避免 UI 误导
            std::cout << "  Reg[" << reg_idx << "] not matched" << std::endl;
        }
        
        result.matches.push_back(m);
    }

    // 7. 处理未匹配的注销人脸（未登记）：used 仍为 false 的列 = 出现在注销现场、
    //    但注册库里找不到对应的人（外来人员/未注册者），单独立账供 UI 红色警示
    for (int cancel_idx = 0; cancel_idx < num_cancelled; ++cancel_idx) {
        if (used[cancel_idx]) continue;
        
        CancellationMatch m;
        m.registration_record_id = 0;   // 0=无对应注册记录
        m.status = 2;  // 未登记
        m.similarity = 0.0f;
        m.cancellation_image = cancelled[cancel_idx].face_photo_path;
        if (m.cancellation_image.empty()) {
            m.cancellation_image = cancelled[cancel_idx].original_photo_path;
        }
        
        result.matches.push_back(m);
        std::cout << "  Cancel[" << cancel_idx << "] unmatched (not registered)" << std::endl;
    }

    auto end = std::chrono::steady_clock::now();
    auto match_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - match_start).count();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    std::cout << "Match complete: " << result.matches.size() << " results" << std::endl;
    std::cout << "Total cancellation time: " << total_duration << "ms "
              << "(detection: " << detect_duration << "ms, "
              << "matching: " << match_duration << "ms)" << std::endl;
    
    return result;  // 仅返回比对结果；用户确认后由 confirmCancellation 落库
}

// 注销结果确认落库：把内存中的匹配明细整体写回（replaceCancellationData 先删后插，
// 允许用户对同一任务反复“重新比对”而不累积脏数据），同时置任务 is_cancelled=1
// 并记录 cancelled_count（status=1 的匹配数，由 UI 统计传入）。
// 前置校验：任务必须存在且未被注销过（与 cancelTask 同样的幂等保护）
bool RollCallService::confirmCancellation(
    int task_id, int cancelled_count,
    const CancellationProcessResult& result) {
    Task t = db_->getTask(task_id);
    if (t.id == 0 || t.is_cancelled) return false;

    // 以下两个循环只是 DTO(展示结构) -> DO(库表结构) 的字段搬运，move 避免复制字符串
    std::vector<CancellationPhotoRecord> photos;
    photos.reserve(result.photos.size());
    for (const auto& source : result.photos) {
        CancellationPhotoRecord photo;
        photo.task_id = task_id;
        photo.original_photo_path = source.original_path;
        photo.processed_photo_path = source.processed_path;
        photos.push_back(std::move(photo));
    }

    std::vector<CancellationMatchRecord> matches;
    matches.reserve(result.matches.size());
    for (const auto& source : result.matches) {
        CancellationMatchRecord match;
        match.task_id = task_id;
        match.registration_record_id = source.registration_record_id;
        match.registration_image = source.registration_image;
        match.cancellation_image = source.cancellation_image;
        match.similarity = source.similarity;
        match.status = source.status;
        matches.push_back(std::move(match));
    }

    // 先写明细、后更新任务：照片/匹配落库失败直接返回 false，任务保持“未注销”可重试
    if (!db_->replaceCancellationData(task_id, photos, matches))
        return false;
    return db_->updateTask(task_id, t.registered_count, cancelled_count, 1);
}

Task RollCallService::getTaskInfo(int task_id) {
    return db_->getTask(task_id);
}

std::vector<Task> RollCallService::getAllTasks() {
    return db_->getAllTasks();
}
