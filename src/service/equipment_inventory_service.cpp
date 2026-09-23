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

EquipmentInventoryService::EquipmentInventoryService(
    std::shared_ptr<RollCallService> roll_call_service)
    : roll_call_service_(std::move(roll_call_service)) {}

bool EquipmentInventoryService::initialize(const std::string& executable_path,
                                           const std::string& model_path,
                                           const std::string& labels_path) {
    return initialize({EquipmentModelConfig{executable_path, model_path, labels_path}});
}

bool EquipmentInventoryService::initialize(const std::vector<EquipmentModelConfig>& models) {
    models_ = models;
    model_label_counts_.clear();
    ready_ = false;
    if (models_.empty()) return false;
    for (const auto& model : models_) {
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
    executable_path_ = models_[0].executable_path;
    model_path_ = models_[0].model_path;
    labels_path_ = models_[0].labels_path;
    label_count_ = model_label_counts_[0];
    ready_ = true;
    return true;
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

bool EquipmentInventoryService::deleteTask(int task_id) {
    return roll_call_service_ && roll_call_service_->deleteTask(task_id);
}

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
    if (!process.waitForStarted(3000)) {
        std::cerr << "Unable to start equipment detector: "
                  << model.executable_path << std::endl;
        return false;
    }
    if (!process.waitForFinished(-1)) {
        process.kill();
        process.waitForFinished(1000);
        return false;
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        std::cerr << "Equipment detector failed, exit code=" << process.exitCode()
                  << " stderr=" << process.readAllStandardError().toStdString() << std::endl;
        return false;
    }
    return parseResultFile(result_path, detections);
}

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
        detection.rect = cv::Rect(x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1));
        detections.push_back(std::move(detection));
    }
    return true;
}

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

    const QFileInfo task_info(QString::fromUtf8(task_folder.c_str()));
    QString task_photo_path = task_info.absoluteFilePath() + "/" +
        QString::fromUtf8(TaskManager::generateUniqueFilename(
            "original_equipment", source_info.suffix().toStdString()).c_str());
    const QString source_absolute = source_info.absoluteFilePath();
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
    for (size_t i = 0; i < models_.size(); ++i) {
        const std::string output_path = TaskManager::generateUniqueFilename(
            "equipment_" + std::to_string(i), "txt");
        const std::string result_path = task_folder + "/" + output_path;
        std::vector<EquipmentDetection> model_detections;
        if (!runDetector(result.original_path, result_path, models_[i],
                         model_label_counts_[i], model_detections)) {
            result.error_message = "设备识别程序执行失败: " + result.original_path;
            unlink(result_path.c_str());
            return result;
        }
        result.detections.insert(result.detections.end(), model_detections.begin(), model_detections.end());
        unlink(result_path.c_str());
    }
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
        if (image.empty()) return {};

        const cv::Rect image_bounds(0, 0, image.cols, image.rows);
        for (const auto& detection : result.detections) {
            const cv::Rect rect = detection.rect & image_bounds;
            if (rect.width <= 0 || rect.height <= 0) continue;
            std::ostringstream text;
            text << detection.label << " " << std::fixed << std::setprecision(2)
                 << detection.confidence;
            cv::rectangle(image, rect, cv::Scalar(0, 255, 0), 3);
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
        std::cerr << "Equipment image processing failed: " << error.what() << std::endl;
        return {};
    }
}

EquipmentTaskResult EquipmentInventoryService::processPhotos(
    int task_id, const std::vector<std::string>& photo_paths, int phase) {
    EquipmentTaskResult result;
    result.task_id = task_id;
    result.phase = phase;
    const Task task = getTaskInfo(task_id);
    if (task.id == 0 || task.folder_path.empty()) {
        result.error_message = "设备任务不存在";
        return result;
    }

    for (const auto& path : photo_paths) {
        EquipmentPhotoResult photo = processSinglePhoto(path, task.folder_path);
        for (const auto& item : photo.counts) result.total_counts[item.first] += item.second;
        if (!photo.success && result.error_message.empty()) result.error_message = photo.error_message;
        result.photos.push_back(std::move(photo));
    }
    if (photo_paths.empty()) result.error_message = "未选择图片";
    result.success = !result.photos.empty() && result.error_message.empty();
    return result;
}

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
            detection.photo_id = static_cast<int>(photo_index);
            detection.class_index = source_detection.class_index;
            detection.label = source_detection.label;
            detection.confidence = source_detection.confidence;
            detection.x1 = source_detection.rect.x;
            detection.y1 = source_detection.rect.y;
            detection.x2 = source_detection.rect.x + source_detection.rect.width;
            detection.y2 = source_detection.rect.y + source_detection.rect.height;
            detections.push_back(std::move(detection));
        }
    }
    if (!roll_call_service_->getDatabase()->replaceEquipmentData(
            result.task_id, result.phase, photos, detections)) return false;

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
