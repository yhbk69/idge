/* 设备盘点服务实现
流程：照片归档复制到任务目录 -> 逐模型进程内 YOLO11Model 推理 ->
     按标签计数 + 画框回显图 -> 用户确认后 saveResult 落库（与点名共库，phase 分场景区）
说明：检测在主进程内完成（与实时预览 PpeTask 同一推理链路），模型实例在
     initialize 时一次性加载常驻，避免每张照片重复 init 的秒级开销。
     本服务串行处理照片（单张推理已吃满所绑 NPU 核，并行只增开销）。
*/
#include "equipment_inventory_service.h"

#include <QFileInfo>
#include <QDebug>

#include <cstdlib>
#include <fstream>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <sys/stat.h>
#include <utility>

#include "task_manager.h"
#include "qt_image_utils.h"
#include "easy_timer.h"   // yolo11_model.hpp 依赖 TIMER（与 ppe_task/frmmain 同序）
#include "yolo11_model.hpp"

// 构造注入：仅保存 shared_ptr，不做任何 IO；ready_=false 前所有流程接口都会拒绝工作
EquipmentInventoryService::EquipmentInventoryService(
    std::shared_ptr<RollCallService> roll_call_service)
    : roll_call_service_(std::move(roll_call_service)) {}

// unique_ptr<YOLO11Model> 需要完整类型才能析构，故在此（include 之后）物化
EquipmentInventoryService::~EquipmentInventoryService() = default;

// 单模型便捷重载：包成 vector 转调多模型版本（两接口共享同一套校验逻辑，行为一致）
bool EquipmentInventoryService::initialize(const std::string& model_path,
                                           const std::string& labels_path) {
    return initialize({EquipmentModelConfig{"", model_path, labels_path}});
}

// 多模型初始化（全部成功才置 ready_，任一项失败保持未就绪并回收已加载模型）：
//   - 标签文件：行数=类别数（容忍 CRLF，逐行去 \r，空行不计数），必须>0 行；
//   - rknn 权重：必须以普通文件形式存在；
//   - 每模型构造常驻 YOLO11Model，绑定核约定：首模型 NPU 核0、其余核1
//     （与设备页预览的核分配一致，主检测流水线留核2）。
// 原子性：新配置全部构建成功才整体替换旧状态（临时容器 → swap）；任一项失败
//   直接返回 false，成员与 ready_ 保持不动——支持"模型热更新失败仍用老模型"。
bool EquipmentInventoryService::initialize(const std::vector<EquipmentModelConfig>& models) {
    if (models.empty()) return false;

    // 全部构建进局部容器，成功前不触碰任何成员（失败即整批丢弃，旧状态无损）
    std::vector<std::unique_ptr<YOLO11Model>> new_detectors;
    std::vector<int> new_counts;
    for (size_t i = 0; i < models.size(); ++i) {
        const auto& model = models[i];
        // 数标签行数：一行一类别，行号即模型输出 class_index（检测结果转换时
        // 按行号取名，这里 count 仅作为解码参数 numClasses 与合法性校验）
        int count = 0;
        std::ifstream labels(model.labels_path);
        if (!labels.is_open()) return false;
        std::string line;
        while (std::getline(labels, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) ++count;
        }
        if (count <= 0 || model.model_path.empty() ||
            !QFileInfo(QString::fromUtf8(model.model_path.c_str())).isFile()) {
            qWarning() << "Equipment inventory: invalid model config at index" << int(i);
            return false;
        }
        const rknn_core_mask core_mask = (i == 0) ? RKNN_NPU_CORE_0 : RKNN_NPU_CORE_1;
        try {
            new_detectors.push_back(std::make_unique<YOLO11Model>(
                model.model_path, model.labels_path, core_mask, count));
        } catch (const std::exception& e) {
            // YOLO11Model 构造失败抛 runtime_error：局部实例随作用域析构，成员未受影响
            qWarning() << "Equipment inventory: model load failed at index" << int(i)
                       << ":" << e.what();
            return false;
        }
        new_counts.push_back(count);
        qInfo() << "Equipment inventory: model loaded" << model.model_path.c_str()
                << "classes =" << count << "npu core" << (i == 0 ? 0 : 1);
    }

    // 全部成功：整体替换。旧 detectors_ 在 swap 后随临时容器析构归还 NPU
    models_ = models;
    model_label_counts_.swap(new_counts);
    detectors_.swap(new_detectors);
    // 保留首模型的便捷副本：兼容早期单模型接口（model_path_ 等成员仍在类内暴露）
    executable_path_ = models_[0].executable_path;
    model_path_ = models_[0].model_path;
    labels_path_ = models_[0].labels_path;
    label_count_ = model_label_counts_[0];
    ready_ = true;
    return true;
}

// 创建设备盘点任务：与点名任务同表不同 type（"equipment_registration"），
// 靠 type 过滤实现两套业务共存于一库；建夹成功但写库失败时回滚删夹，规则与点名一致
int EquipmentInventoryService::createEquipmentTask(const std::string& task_name) {
    if (!roll_call_service_ || !roll_call_service_->getDatabase()) return -1;
    const std::string base_path = roll_call_service_->getStoragePath();
    const std::string folder = TaskManager::createTaskFolder(base_path, task_name);
    if (folder.empty()) return -1;
    const int task_id = roll_call_service_->getDatabase()->createTask(
        task_name, "equipment_registration", folder);
    if (task_id < 0) TaskManager::deleteTaskFolder(folder);
    return task_id;
}

std::vector<Task> EquipmentInventoryService::getEquipmentTasks() const {
    if (!roll_call_service_ || !roll_call_service_->getDatabase()) return {};
    return roll_call_service_->getDatabase()->getTasksByType("equipment_registration");
}

Task EquipmentInventoryService::getTaskInfo(int task_id) const {
    if (!roll_call_service_ || !roll_call_service_->getDatabase()) return Task{};
    return roll_call_service_->getDatabase()->getTask(task_id);
}

// 删除直接委托点名服务：文件夹/库记录/级联明细的删除规则与其完全一致，不再重复实现
bool EquipmentInventoryService::deleteTask(int task_id) {
    return roll_call_service_ && roll_call_service_->deleteTask(task_id);
}

// 进程内跑一次检测（models_[model_index] 对应的常驻 YOLO11Model）：
// read_image 用 malloc/stbi 分配缓冲，调用方负责 free(virt_addr)；
// detect 输出的 box 已经过 letterbox 逆变换，是原图像素坐标，直接采用。
// cls_id 越界（标签表与模型输出不一致的脏配置）的检出整条丢弃，防止空标签入库
bool EquipmentInventoryService::runDetector(
    const std::string& image_path, size_t model_index,
    std::vector<EquipmentDetection>& detections) {
    if (model_index >= detectors_.size() || !detectors_[model_index]) return false;
    YOLO11Model& detector = *detectors_[model_index];

    image_buffer_t img{};   // 含内部 shared_ptr 语义成员，禁止 memset
    if (read_image(image_path.c_str(), &img) != 0 || img.virt_addr == NULL) {
        std::cerr << "Unable to decode image for equipment detect: " << image_path << std::endl;
        return false;
    }
    object_detect_result_list results;
    detector.detect(&img, &results);   // infer 内部先 memset 再填充
    free(img.virt_addr);

    const std::vector<std::string>& names = detector.getClassNames();
    for (int i = 0; i < results.count; ++i) {
        const object_detect_result& r = results.results[i];
        if (r.cls_id < 0 || r.cls_id >= static_cast<int>(names.size())) continue;
        EquipmentDetection detection;
        detection.class_index = r.cls_id;
        detection.confidence = r.prop;
        detection.label = names[r.cls_id];
        // 框用 (x1,y1,w,h) 存储；max(0,·) 兜底坐标倒置的异常检测框，
        // 绘制阶段再与图像边界求交并二次剔除零宽高框
        detection.rect = cv::Rect(r.box.left, r.box.top,
                                  std::max(0, r.box.right - r.box.left),
                                  std::max(0, r.box.bottom - r.box.top));
        detections.push_back(std::move(detection));
    }
    return true;
}

// 单张照片完整盘点链：归档复制 -> 多模型检测 -> 计数 -> 画框回显。
// 返回 success=false 时 error_message 已带中文原因，供 UI 弹窗
EquipmentPhotoResult EquipmentInventoryService::processSinglePhoto(
    const std::string& photo_path, const std::string& task_folder) {
    EquipmentPhotoResult result;
    const QFileInfo source_info(QString::fromUtf8(photo_path.c_str()));
    std::cerr << "[equipment] processSinglePhoto input=" << photo_path
              << " exists=" << source_info.exists() << " size=" << source_info.size()
              << " suffix=" << source_info.suffix().toStdString() << std::endl;
    if (!source_info.exists() || !source_info.isFile()) {
        result.error_message = "无法读取原始图片: " + photo_path;
        return result;
    }

    // 路径约定：UI 与落库都只看“任务目录内”的副本，
    // 把用户选择的照片复制归档（unique 命名防同名覆盖），result.original_path
    // 从此指向任务目录副本——删任务即全清，不依赖用户原始文件继续存在
    const QFileInfo task_info(QString::fromUtf8(task_folder.c_str()));
    QString task_photo_path = task_info.absoluteFilePath() + "/" +
        QString::fromUtf8(TaskManager::generateUniqueFilename(
            "original_equipment", source_info.suffix().toStdString()).c_str());
    const QString source_absolute = source_info.absoluteFilePath();
    // 照片本来就在任务目录里（重跑场景）：跳过复制，避免自我复制报错
    if (source_absolute != QFileInfo(task_photo_path).absoluteFilePath()) {
        if (!QFile::copy(source_absolute, task_photo_path)) {
            result.error_message = "无法复制原始图片到任务目录: " + photo_path;
            return result;
        }
    }
    result.original_path = task_photo_path.toUtf8().toStdString();
    std::cerr << "[equipment] copied original=" << result.original_path
              << " exists=" << QFileInfo(QString::fromUtf8(result.original_path.c_str())).exists()
              << " size=" << QFileInfo(QString::fromUtf8(result.original_path.c_str())).size() << std::endl;
    // 逐模型串行检测：结果按模型顺序拼接进同一 detections；
    // 任一模型失败即整照失败（保守策略：宁可全弃不可部分统计误导盘点数）
    for (size_t i = 0; i < models_.size(); ++i) {
        std::vector<EquipmentDetection> model_detections;
        if (!runDetector(result.original_path, i, model_detections)) {
            result.error_message = "设备检测执行失败: " + result.original_path;
            return result;
        }
        result.detections.insert(result.detections.end(), model_detections.begin(), model_detections.end());
    }
    // 按标签计数：operator[] 自动建 0 再 ++，即“标签->数量”直方图
    for (const auto& detection : result.detections)
        ++result.counts[detection.label];
    result.processed_path = drawAndSave(result, task_folder);
    std::cerr << "[equipment] processed output=" << result.processed_path
              << " exists=" << QFileInfo(QString::fromUtf8(result.processed_path.c_str())).exists()
              << " size=" << QFileInfo(QString::fromUtf8(result.processed_path.c_str())).size() << std::endl;
    if (result.processed_path.empty()) {
        result.error_message = "无法读取或保存后处理图片: " + result.original_path;
        return result;
    }
    result.success = true;
    return result;
}

// 画框回显：与注册流程共用 loadPixmapSafe 解码（OpenCV 解码失败退回 ImageMagick）。
// 像素流向：QPixmap -> QImage(RGB888) -> cv::Mat 视图(以 bytesPerLine 作行距) -> 转 BGR 再画。
// cv::Mat 只是 rgb 缓冲的浅层视图（constBits 去 const），cvtColor 立即深拷贝到 image，
// rgb 出作用域前完成物化，无悬垂指针风险
std::string EquipmentInventoryService::drawAndSave(
    const EquipmentPhotoResult& result, const std::string& task_folder) const {
    try {
        std::cerr << "[equipment] draw input=" << result.original_path << std::endl;
        // Match the registration flow's image loading path. Some board images
        // cannot be decoded by OpenCV's JPEG backend, while Qt/ImageMagick can.
        const QPixmap decoded = loadPixmapSafe(QString::fromUtf8(result.original_path.c_str()));
        cv::Mat image;
        if (!decoded.isNull()) {
            const QImage rgb = decoded.toImage().convertToFormat(QImage::Format_RGB888);
            cv::Mat rgb_view(rgb.height(), rgb.width(), CV_8UC3,
                             const_cast<uchar*>(rgb.constBits()), rgb.bytesPerLine());
            cv::cvtColor(rgb_view, image, cv::COLOR_RGB2BGR);
        }
        std::cerr << "[equipment] draw decoded image empty=" << image.empty();
        if (!image.empty()) std::cerr << " size=" << image.cols << "x" << image.rows;
        std::cerr << std::endl;
        if (image.empty()) return {};   // 解码失败：返回空串，上层判该照片失败

        const cv::Rect image_bounds(0, 0, image.cols, image.rows);
        for (const auto& detection : result.detections) {
            // 与图像边界求交剔除越界框；求交后非正的宽高说明框完全在图外，跳过
            const cv::Rect rect = detection.rect & image_bounds;
            if (rect.width <= 0 || rect.height <= 0) continue;
            std::ostringstream text;
            text << detection.label << " " << std::fixed << std::setprecision(2)
                 << detection.confidence;
            cv::rectangle(image, rect, cv::Scalar(0, 255, 0), 3);
            // 标签文字画框上方 8px；max(20,·) 防框贴顶时文字被裁出画面
            cv::putText(image, text.str(), cv::Point(rect.x, std::max(20, rect.y - 8)),
                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
        }

        const std::string filename = TaskManager::generateUniqueFilename("processed_equipment", "png");
        const std::string path = task_folder + "/" + filename;
        const bool saved = cv::imwrite(path, image);
        std::cerr << "[equipment] draw imwrite path=" << path << " saved=" << saved
                  << " exists=" << QFileInfo(QString::fromUtf8(path.c_str())).exists() << std::endl;
        if (!saved) return {};
        return path;
    } catch (const cv::Exception& error) {
        // OpenCV 断言/内存异常兜底：单照绘制失败降级为“该照片失败”，不炸整批
        std::cerr << "Equipment image processing failed: " << error.what() << std::endl;
        return {};
    }
}

// 任务级盘点入口：串行逐张处理（单张推理已占满所绑 NPU 核，再并行只增开销）。
// 部分成功语义：一张失败不中断其余照片，但整体 success 置 false 并保留首个错误；
// UI 依据 success 决定是否放行 saveResult，避免把缺照的统计当完整盘点入库
EquipmentTaskResult EquipmentInventoryService::processPhotos(
    int task_id, const std::vector<std::string>& photo_paths, int phase) {
    EquipmentTaskResult result;
    result.task_id = task_id;
    result.phase = phase;   // 0=进场/登记盘点，1=退场/复查盘点，落库按阶段分区互不覆盖
    const Task task = getTaskInfo(task_id);
    if (task.id == 0 || task.folder_path.empty()) {
        result.error_message = "设备任务不存在";
        return result;
    }

    for (const auto& path : photo_paths) {
        EquipmentPhotoResult photo = processSinglePhoto(path, task.folder_path);
        // 各照片直方图并入任务级 total_counts（同标签跨照片累加=品类总数）
        for (const auto& item : photo.counts) result.total_counts[item.first] += item.second;
        if (!photo.success && result.error_message.empty()) result.error_message = photo.error_message;
        result.photos.push_back(std::move(photo));
    }
    if (photo_paths.empty()) result.error_message = "未选择图片";
    result.success = !result.photos.empty() && result.error_message.empty();
    return result;
}

// 盘点结果落库（确认后调用）：
//   - 仅接受 success 且非空的结果（防半成品入库）；
//   - replaceEquipmentData 按 (task_id, phase) 原子替换：重跑同阶段自动覆盖旧账；
//   - detection.photo_id 填的是 photos 数组下标而非真实 rowid，由 DB 层在事务内
//     插入照片拿到新 rowid 后统一重映射（下标越界会被 DB 层判脏数据整批回滚）
bool EquipmentInventoryService::saveResult(const EquipmentTaskResult& result) {
    if (!roll_call_service_ || !roll_call_service_->getDatabase()) return false;
    if (!result.success || result.photos.empty()) return false;
    std::vector<EquipmentPhotoRecord> photos;
    std::vector<EquipmentDetectionRecord> detections;
    for (size_t photo_index = 0; photo_index < result.photos.size(); ++photo_index) {
        const auto& source = result.photos[photo_index];
        EquipmentPhotoRecord photo;
        photo.task_id = result.task_id;
        photo.phase = result.phase;
        photo.original_photo_path = source.original_path;
        photo.processed_photo_path = source.processed_path;
        photos.push_back(std::move(photo));
        for (const auto& source_detection : source.detections) {
            EquipmentDetectionRecord detection;
            detection.photo_id = static_cast<int>(photo_index);   // 暂存数组下标，见上注
            detection.class_index = source_detection.class_index;
            detection.label = source_detection.label;
            detection.confidence = source_detection.confidence;
            // cv::Rect(w,h) 回转成库表存储的角点式 (x1,y1,x2,y2)
            detection.x1 = source_detection.rect.x;
            detection.y1 = source_detection.rect.y;
            detection.x2 = source_detection.rect.x + source_detection.rect.width;
            detection.y2 = source_detection.rect.y + source_detection.rect.height;
            detections.push_back(std::move(detection));
        }
    }
    if (!roll_call_service_->getDatabase()->replaceEquipmentData(
            result.task_id, result.phase, photos, detections)) return false;

    // 明细落库成功后才更新任务计数：total=各标签数量之和（含多张同类的累加）。
    // phase 决定写哪个字段：0 -> registered_count（is_cancelled 归 0，重开新盘点）；
    //                        1 -> cancelled_count 并置 is_cancelled=1（该阶段已复查）
    Task task = getTaskInfo(result.task_id);
    const int total = std::accumulate(
        result.total_counts.begin(), result.total_counts.end(), 0,
        [](int sum, const auto& item) { return sum + item.second; });
    if (result.phase == 0) {
        return roll_call_service_->getDatabase()->updateTask(
            result.task_id, total, task.cancelled_count, 0);
    }
    return roll_call_service_->getDatabase()->updateTask(
        result.task_id, task.registered_count, total, 1);
}

std::vector<EquipmentPhotoRecord> EquipmentInventoryService::getPhotos(int task_id, int phase) const {
    if (!roll_call_service_ || !roll_call_service_->getDatabase()) return {};
    return roll_call_service_->getDatabase()->getEquipmentPhotosByTask(task_id, phase);
}

std::vector<EquipmentDetectionRecord>
EquipmentInventoryService::getDetections(int photo_id) const {
    if (!roll_call_service_ || !roll_call_service_->getDatabase()) return {};
    return roll_call_service_->getDatabase()->getEquipmentDetectionsByPhoto(photo_id);
}
