/* 设备盘点服务实现
流程：照片归档复制到任务目录 -> 逐模型起子进程跑 YOLO11 RKNN 检测 -> 解析结果文本 ->
     按标签计数 + 画框回显图 -> 用户确认后 saveResult 落库（与点名共库，phase 分场景区）
说明：检测走外部可执行程序而非进程内推理，以“结果文件”为 IPC 载体，
     进程隔离保证模型/驱动崩溃不拖垮 UI 主程序。本服务串行处理照片（子进程本身吃满 NPU）。
*/
#include "equipment_inventory_service.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTextStream>

#include <fstream>
#include <algorithm>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

#include "../utils/task_manager.h"
#include "../utils/qt_image_utils.h"

// 构造注入：仅保存 shared_ptr，不做任何 IO；ready_=false 前所有流程接口都会拒绝工作
EquipmentInventoryService::EquipmentInventoryService(
    std::shared_ptr<RollCallService> roll_call_service)
    : roll_call_service_(std::move(roll_call_service)) {}

// 单模型便捷重载：包成 vector 转调多模型版本（两接口共享同一套校验逻辑，行为一致）
bool EquipmentInventoryService::initialize(const std::string& executable_path,
                                           const std::string& model_path,
                                           const std::string& labels_path) {
    return initialize({EquipmentModelConfig{executable_path, model_path, labels_path}});
}

// 多模型初始化校验（全部通过才置 ready_，任一项失败保持未就绪、不留半初始化状态）：
//   - 标签文件：行数=类别数（容忍 CRLF，逐行去 \r，空行不计数），必须>0 行；
//   - exe / rknn：必须以普通文件形式存在；
//   - 每模型的类别数存入 model_label_counts_，与 models_ 严格同序，供子进程调参使用。
bool EquipmentInventoryService::initialize(const std::vector<EquipmentModelConfig>& models) {
    models_ = models;
    model_label_counts_.clear();
    ready_ = false;   // 先复位：重复调用 initialize 重新校验时可安全换配置
    if (models_.empty()) return false;
    for (const auto& model : models_) {
        // 数标签行数：一行一类别，行号即模型输出 class_index（解析结果时不再回查标签表，
        // 标签名直接取结果文件里的 label 字段，这里 count 仅作为解码参数与合法性校验）
        int count = 0;
        std::ifstream labels(model.labels_path);
        if (!labels.is_open()) return false;
        std::string line;
        while (std::getline(labels, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) ++count;
        }
        if (count <= 0 || model.executable_path.empty() || model.model_path.empty() ||
            !QFileInfo(QString::fromUtf8(model.executable_path.c_str())).isFile() ||
            !QFileInfo(QString::fromUtf8(model.model_path.c_str())).isFile()) return false;
        model_label_counts_.push_back(count);
    }
    // 保留首模型的便捷副本：兼容早期单模型接口（executable_path_ 等成员仍在类内暴露）
    executable_path_ = models_[0].executable_path;
    model_path_ = models_[0].model_path;
    labels_path_ = models_[0].labels_path;
    label_count_ = model_label_counts_[0];
    ready_ = true;
    return true;
/* 以下为“单模型时代”的旧实现（含更详细的错误日志），迁移到多模型版本后
   整段注释保留备查；逻辑已由上方多模型循环等价覆盖，勿再启用 */
/*
    executable_path_ = executable_path;
    model_path_ = model_path;
    labels_path_ = labels_path;
    label_count_ = 0;
    ready_ = false;

    std::ifstream labels(labels_path_);
    if (!labels.is_open()) {
        std::cerr << "Unable to open equipment labels: " << labels_path_ << std::endl;
        return false;
    }
    std::string line;
    while (std::getline(labels, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) ++label_count_;
    }
    if (label_count_ <= 0) {
        std::cerr << "Equipment label file is empty: " << labels_path_ << std::endl;
        return false;
    }
    if (executable_path_.empty() || model_path_.empty()) return false;
    if (!QFileInfo(QString::fromUtf8(executable_path_.c_str())).isFile()) {
        std::cerr << "Equipment detector executable not found: " << executable_path_ << std::endl;
        return false;
    }
    if (!QFileInfo(QString::fromUtf8(model_path_.c_str())).isFile()) {
        std::cerr << "Equipment model not found: " << model_path_ << std::endl;
        return false;
    }
    ready_ = true;
    return true;
*/
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

// 以子进程方式运行一次设备检测（IPC 契约：4 个位置参数 + 结果文本文件）。
// 参数顺序由 rknn_yolo11_demo 约定：<模型文件> <图片> <结果txt> <类别数>；
// 结果写文件而非 stdout：demo 运行日志会混在 stdout，单独文件保证解析纯净。
// 工作目录切到 exe 所在目录：demo 可能以相对路径加载其同级的运行库/标定文件
bool EquipmentInventoryService::runDetector(
    const std::string& image_path, const std::string& result_path,
    const EquipmentModelConfig& model, int label_count,
    std::vector<EquipmentDetection>& detections) const {
    if (model.executable_path.empty() || model.model_path.empty() || label_count <= 0)
        return false;

    QProcess process;
    process.setProgram(QString::fromUtf8(model.executable_path.c_str()));
    process.setWorkingDirectory(QFileInfo(QString::fromUtf8(model.executable_path.c_str())).absolutePath());
    process.setArguments({
        QString::fromUtf8(model.model_path.c_str()),
        QString::fromUtf8(image_path.c_str()),
        QString::fromUtf8(result_path.c_str()),
        QString::number(label_count)
    });
    process.start();
    // 3000ms 启动时限：exe 缺失/不可执行应立刻失败，不值得久等（QProcess 失败也常报 started）
    if (!process.waitForStarted(3000)) {
        std::cerr << "Unable to start equipment detector: "
                  << model.executable_path << std::endl;
        return false;
    }
    // -1=不限时：RKNN 首次加载模型可能数十秒，宁可等待不可误杀；
    // 但若进程假死将永远卡住本函数（串行流程可 UI 层超时兜底，注释留此风险提示）
    if (!process.waitForFinished(-1)) {
        process.kill();               // 异常终止路径：先杀再有限等待回收，不留僵尸进程
        process.waitForFinished(1000);
        return false;
    }
    // 双重退出校验：异常退出（崩溃/信号杀）与非零退出码都视为检测失败
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::cerr << "Equipment detector failed, exit code=" << process.exitCode()
                  << " stderr=" << process.readAllStandardError().toStdString() << std::endl;
        return false;
    }
    return parseResultFile(result_path, detections);
}

// 解析 demo 输出的结果文件：约定每行一条定长 JSON 对象（非标准 JSON 数组，逐行更稳）。
// 正则严格锚定整行并允许字段间任意空白：格式漂移的行（日志混入/半行截断）
// 直接跳过而不是报错——个别坏行不应否定整次检测的其余有效结果
bool EquipmentInventoryService::parseResultFile(
    const std::string& result_path,
    std::vector<EquipmentDetection>& detections) const {
    QFile file(QString::fromUtf8(result_path.c_str()));
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return false;

    const QRegularExpression expression(
        R"REGEX(^\s*\{\s*"index"\s*:\s*(-?\d+)\s*,\s*"confidence"\s*:\s*([-+0-9.eE]+)\s*,\s*"label"\s*:\s*"([^"]*)"\s*,\s*"x1"\s*:\s*(-?\d+)\s*,\s*"y1"\s*:\s*(-?\d+)\s*,\s*"x2"\s*:\s*(-?\d+)\s*,\s*"y2"\s*:\s*(-?\d+)\s*\}\s*$)REGEX");

    QTextStream stream(&file);
    while (!stream.atEnd()) {
        const QString line = stream.readLine();
        const QRegularExpressionMatch match = expression.match(line);
        if (!match.hasMatch()) continue;
        EquipmentDetection detection;
        detection.class_index = match.captured(1).toInt();
        detection.confidence = match.captured(2).toFloat();
        detection.label = match.captured(3).toUtf8().toStdString();
        const int x1 = match.captured(4).toInt();
        const int y1 = match.captured(5).toInt();
        const int x2 = match.captured(6).toInt();
        const int y2 = match.captured(7).toInt();
        // 框用 (x1,y1,w,h) 存储；max(0,·) 兜底坐标倒置（x2<x1）的异常检测框，
        // 绘制阶段再与图像边界求交并二次剔除零宽高框
        detection.rect = cv::Rect(x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1));
        detections.push_back(std::move(detection));
    }
    return true;
}

// 单张照片完整盘点链：归档复制 -> 多模型检测 -> 计数 -> 画框回显。
// 返回 success=false 时 error_message 已带中文原因，供 UI 弹窗
EquipmentPhotoResult EquipmentInventoryService::processSinglePhoto(
    const std::string& photo_path, const std::string& task_folder) const {
    EquipmentPhotoResult result;
    const QFileInfo source_info(QString::fromUtf8(photo_path.c_str()));
    std::cerr << "[equipment] processSinglePhoto input=" << photo_path
              << " exists=" << source_info.exists() << " size=" << source_info.size()
              << " suffix=" << source_info.suffix().toStdString() << std::endl;
    if (!source_info.exists() || !source_info.isFile()) {
        result.error_message = "无法读取原始图片: " + photo_path;
        return result;
    }

    // 路径约定：外部检测程序与后续 UI 都只看“任务目录内”的副本，
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
        const std::string output_path = TaskManager::generateUniqueFilename(
            "equipment_" + std::to_string(i), "txt");
        const std::string result_path = task_folder + "/" + output_path;
        std::vector<EquipmentDetection> model_detections;
        if (!runDetector(result.original_path, result_path, models_[i],
                         model_label_counts_[i], model_detections)) {
            result.error_message = "设备识别程序执行失败: " + result.original_path;
            unlink(result_path.c_str());   // 失败也要清理半成品结果文件
            return result;
        }
        result.detections.insert(result.detections.end(), model_detections.begin(), model_detections.end());
        unlink(result_path.c_str());       // 检测结果已解析进内存，临时 txt 即刻删除防堆积
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

// 任务级盘点入口：串行逐张处理（外部 demo 子进程已占满 NPU，再并行只增开销）。
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
