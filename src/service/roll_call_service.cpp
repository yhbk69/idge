/* 核心业务逻辑层
人脸检测和特征提取：调用 RKNN 模型（NPU 加速）
去重算法：通过余弦相似度判断人脸是否重复
图像处理：绘制人脸框、裁剪人脸区域、保存图片
任务管理：创建/删除任务，保存识别结果到数据库
注销匹配：将注销照片与已登记人脸进行一对一匹配。 
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
    return image;
}
}

RollCallService::RollCallService()
    : recognizer_(std::make_unique<FaceRecognitionWrapper>()),
      db_(std::make_unique<BusinessDBManager>()),
      thread_pool_(std::make_unique<ThreadPool>()),
      similarity_threshold_(0.8f) {
}//这里0.8表明相似度在这个以上就认为重复

RollCallService::~RollCallService() = default;

bool RollCallService::initialize(const std::string& exe_path,
                                 const std::string& det_model_path,
                                 const std::string& rec_model_path,
                                 const std::string& db_path,
                                 const std::string& base_storage_path) {
    base_storage_path_ = base_storage_path;
    detection_model_path_ = det_model_path;
    
    // 创建存储目录
    mkdir(base_storage_path_.c_str(), 0755);
    
    // 配置识别器
    recognizer_->setExecutablePath(exe_path);
    recognizer_->setDetectionModel(det_model_path);
    recognizer_->setRecognitionModel(rec_model_path);
    recognizer_->setThresholds(0.6f, 0.4f);  // 可配置
    
    // 打开数据库
    if (!db_->open(db_path)) {
        std::cerr << "Failed to open database" << std::endl;
        return false;
    }
    
    std::cout << "RollCallService initialized successfully" << std::endl;
    return true;
}

// +++++ 新增：L2归一化
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

// +++++ 新增：向量化的相似度查找（替换原来的循环版本）
int RollCallService::findSimilarFaceVectorized(
    const std::vector<float>& feature,
    const std::vector<std::vector<float>>& unique_features) {
    
    if (unique_features.empty()) {
        return -1;
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

    // 相似度即归一化向量的点积
    std::vector<float> similarities(num_registered);
    for (int i = 0; i < num_registered; ++i) {
        float s = 0.f;
        for (int j = 0; j < feature_dim; ++j) {
            s += registered_matrix[i][j] * query[j];
        }
        similarities[i] = s;
    }
    
    // 找最大值
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
    
    return best_idx;
}

int RollCallService::createRegistrationTask(const std::string& task_name) {
    // 创建任务文件夹
    std::string task_folder = TaskManager::createTaskFolder(base_storage_path_, task_name);
    if (task_folder.empty()) {
        std::cerr << "Failed to create task folder" << std::endl;
        return -1;
    }
    
    // 在数据库中创建任务记录
    int task_id = db_->createTask(task_name, "registration", task_folder);
    if (task_id < 0) {
        std::cerr << "Failed to create task in database" << std::endl;
        TaskManager::deleteTaskFolder(task_folder);
        return -1;
    }
    
    std::cout << "Created registration task: " << task_name << " (ID: " << task_id << ")" << std::endl;
    return task_id;
}

// +++++ 新增：并行版本的单张图片处理（只做检测，不做去重）
PhotoProcessResult RollCallService::processSinglePhotoParallel(
    const std::string& photo_path,
    const std::string& task_folder) {
    
    PhotoProcessResult result;
    result.original_path = photo_path;
    result.unique_count = 0;
    
    std::cerr << "[rollcall] Processing photo (parallel): " << photo_path << std::endl;
    
    // 调用exe检测人脸
    auto start = std::chrono::steady_clock::now();
    FaceDetectionResult det_result = recognizer_->detectAndExtract(photo_path);
    auto end = std::chrono::steady_clock::now();
    
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    std::cout << "  Detected " << det_result.num_faces << " faces in " 
              << duration << "ms" << std::endl;
    
    // 暂时保存所有检测到的人脸（不做去重，稍后统一处理）
    for (const auto& detected_face : det_result.faces) {
        ProcessedFace face;
        face.rect = detected_face.bbox;
        face.face_index = detected_face.face_id;
        face.feature = detected_face.feature;
        face.score = detected_face.score;
        face.is_duplicate = false;  // 暂时标记，后续去重时更新
        face.similar_to_index = -1;
        
        result.faces.push_back(face);
    }
    
    return result;
}

// +++++ 新增：合并所有图片的结果并去重
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
                
                // 保存人脸裁剪图
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
        
        // 绘制后处理图（带人脸框）
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

// +++++ 重构：processPhotos 使用并行处理
TaskProcessResult RollCallService::processPhotos(
    int task_id, const std::vector<std::string>& photo_paths) {
    
    Task task = db_->getTask(task_id);
    if (task.id == 0) {
        std::cerr << "Task not found: " << task_id << std::endl;
        return TaskProcessResult{};
    }
    
    const size_t num_photos = photo_paths.size();
    std::cout << "Starting parallel processing of " << num_photos << " photos..." << std::endl;
    
    auto start = std::chrono::steady_clock::now();
    
    // +++++ 1. 并行检测所有图片的人脸（最多4个同时进行）
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
    
    // +++++ 2. 等待所有检测完成
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
    
    // +++++ 3. 串行去重（去重速度很快，不需要并行）
    TaskProcessResult result = mergeAndDeduplicateResults(
        task_id, task.folder_path, photo_results);
    
    auto end = std::chrono::steady_clock::now();
    auto total_duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        end - start).count();
    
    std::cout << "Total processing time: " << total_duration << "ms "
              << "(detection: " << detect_duration << "ms, "
              << "dedup: " << (total_duration - detect_duration) << "ms)" << std::endl;
    
    return result;
}


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
    
    // 生成文件名
    std::string filename = TaskManager::generateUniqueFilename("processed_" + original_filename, "png");
    std::string save_path = task_folder + "/" + filename;
    
    // 保存图像
    QImage qimg(result_image.data, result_image.cols, result_image.rows, (int)result_image.step, QImage::Format_BGR888);
    if (qimg.copy().save(QString::fromStdString(save_path), "PNG")) {
        std::cout << "  Saved processed image: " << save_path << std::endl;
        return save_path;
    }
    
    std::cerr << "  Failed to save processed image" << std::endl;
    return "";
}

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
        return "";
    }
    
    // 生成文件名
    std::string filename = name_prefix + "_" + std::to_string(face_id) + ".png";
    std::string save_path = task_folder + "/" + filename;
    
    // 保存人脸图
    std::cerr << "[rollcall] writing face path=" << save_path << std::endl;
    // Some board images have a crashing OpenCV JPEG encoder. Keep the
    // registration flow alive; the processed/original images are still saved.
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
            record.feature = face.feature;
            record.original_photo_path = photo.original_path;
            record.processed_photo_path = photo.processed_path;
            record.face_index = face.face_index;
            record.is_duplicate = face.is_duplicate ? 1 : 0;
            
            if (face.is_duplicate) {
                // 重复人脸，指向原始人脸的record_id
                record.similar_to_id = unique_index_to_record_id[face.similar_to_index];
                record.face_photo_path = "";
            } else {
                // 唯一人脸
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
    
    // 更新任务统计信息
    bool success = db_->updateTask(result.task_id, result.total_unique_count, 0, 0);
    
    if (success) {
        std::cout << "Task result saved successfully" << std::endl;
    } else {
        std::cerr << "Failed to save task result" << std::endl;
    }
    
    return success;
}

bool RollCallService::deleteTask(int task_id) {
    // 获取任务信息
    Task task = db_->getTask(task_id);
    if (task.id == 0) {
        return false;
    }
    
    // 删除文件夹
    if (!task.folder_path.empty()) {
        TaskManager::deleteTaskFolder(task.folder_path);
    }
    
    // 删除数据库记录
    return db_->deleteTask(task_id);
}

bool RollCallService::cancelTask(int task_id) {
    Task task = db_->getTask(task_id);
    if (task.id == 0 || task.is_cancelled) return false;
    return db_->updateTask(task_id, task.registered_count,
                           task.cancelled_count, 1);
}

// +++++ 同样优化 matchCancellation 的检测部分
CancellationProcessResult RollCallService::matchCancellation(
    int task_id, const std::vector<std::string>& photo_paths) {
    
    CancellationProcessResult result;
    const Task task = db_->getTask(task_id);
    if (task.id == 0) return result;

    // 1. 获取登记人脸
    auto registered = db_->getUniqueFacesByTask(task_id);
    if (registered.empty()) {
        std::cerr << "No registered faces for task " << task_id << std::endl;
        return result;
    }

    const int num_registered = registered.size();
    const int feature_dim = 512;
    
    std::cout << "Loaded " << num_registered << " registered faces" << std::endl;
    std::cout << "Starting parallel detection of " << photo_paths.size() 
              << " cancellation photos..." << std::endl;

    auto start = std::chrono::steady_clock::now();

    // +++++ 2. 并行检测注销照片
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
    
    // 等待检测完成
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

    // +++++ 3. 汇总所有检测到的注销人脸
    std::vector<FaceRecord> cancelled;
    int cancellation_face_index = 0;
    
    for (auto& photo_result : photo_results) {
        cv::Mat image = loadImageWithFallback(photo_result.original_path, "cancel");
        std::vector<ProcessedFace> processed_faces;
        
        for (const auto& face : photo_result.faces) {
            FaceRecord r;
            r.feature = face.feature;
            r.original_photo_path = photo_result.original_path;
            
            // 保存裁剪的人脸图
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
        
        // 绘制后处理图
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
        return result;
    }

    // +++++ 4. 构建特征矩阵（归一化）
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

    // +++++ 5. 相似度即归一化向量的点积（按需计算配对）
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

    // +++++ 6. 贪心匹配
    std::vector<bool> used(num_cancelled, false);
    
    for (int reg_idx = 0; reg_idx < num_registered; ++reg_idx) {
        const auto& reg = registered[reg_idx];
        
        CancellationMatch m;
        m.registration_record_id = reg.id;
        m.registration_image = reg.face_photo_path;
        
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
            used[best_cancel_idx] = true;
            m.status = 1;  // 注销匹配
            m.similarity = best_score;
            m.cancellation_image = cancelled[best_cancel_idx].face_photo_path;
            if (m.cancellation_image.empty()) {
                m.cancellation_image = cancelled[best_cancel_idx].original_photo_path;
            }
            
            std::cout << "  Matched reg[" << reg_idx << "] with cancel[" 
                      << best_cancel_idx << "] similarity=" << best_score << std::endl;
        } else {
            m.status = 0;  // 未注销
            m.similarity = 0.0f;
            std::cout << "  Reg[" << reg_idx << "] not matched" << std::endl;
        }
        
        result.matches.push_back(m);
    }

    // +++++ 7. 处理未匹配的注销人脸（未登记）
    for (int cancel_idx = 0; cancel_idx < num_cancelled; ++cancel_idx) {
        if (used[cancel_idx]) continue;
        
        CancellationMatch m;
        m.registration_record_id = 0;
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
    
    return result;
}

bool RollCallService::confirmCancellation(
    int task_id, int cancelled_count,
    const CancellationProcessResult& result) {
    Task t = db_->getTask(task_id);
    if (t.id == 0 || t.is_cancelled) return false;

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
