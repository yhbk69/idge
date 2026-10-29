/* 蔡超添加
任务管理工具
主要用于人员点名和设备盘点模块
负责创建任务文件夹、生成唯一文件名、保存图片到指定目录 
*/
#include "task_manager.h"
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <ctime>
#include <cstdlib>
#include <sstream>
#include <iomanip>
#include <opencv2/opencv.hpp>

std::string TaskManager::createTaskFolder(const std::string& base_path, const std::string& task_name) {
    // 确保基础路径存在
    mkdir(base_path.c_str(), 0755);
    
    // 生成任务文件夹路径（使用任务名作为文件夹名，替换特殊字符）
    std::string safe_name = task_name;
    for (char& c : safe_name) {
        if (c == ' ' || c == ':' || c == '/') {
            c = '_';
        }
    }
    
    std::string folder_path = base_path + "/" + safe_name;
    
    // 如果文件夹已存在，添加数字后缀
    int suffix = 1;
    std::string final_path = folder_path;
    while (access(final_path.c_str(), F_OK) == 0) {
        final_path = folder_path + "_" + std::to_string(suffix++);
    }
    
    // 创建文件夹
    if (mkdir(final_path.c_str(), 0755) != 0) {
        return "";
    }
    
    return final_path;
}

std::string TaskManager::generateUniqueFilename(const std::string& prefix, const std::string& extension) {
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count();
    
    int random_num = rand() % 10000;
    
    // Callers historically passed both "jpg" and ".jpg". Keep generated
    // filenames valid for OpenCV/Qt by normalizing the separator here.
    const std::string normalized_extension =
        (!extension.empty() && extension.front() == '.') ? extension : "." + extension;
    std::ostringstream oss;
    oss << prefix << "_" << timestamp << "_" << std::setfill('0') << std::setw(4)
        << random_num << normalized_extension;
    return oss.str();
}

std::string TaskManager::saveImageToFolder(const std::string& folder_path,
                                           const void* image_data,
                                           int width, int height, int channels,
                                           const std::string& filename) {
    std::string full_path = folder_path + "/" + filename;
    
    // 使用OpenCV保存图片
    cv::Mat img;
    if (channels == 3) {
        img = cv::Mat(height, width, CV_8UC3, (void*)image_data).clone();
    } else if (channels == 1) {
        img = cv::Mat(height, width, CV_8UC1, (void*)image_data).clone();
    } else {
        return "";
    }
    
    if (cv::imwrite(full_path, img)) {
        return full_path;
    }
    
    return "";
}

bool TaskManager::deleteTaskFolder(const std::string& folder_path) {
    DIR* dir = opendir(folder_path.c_str());
    if (!dir) return false;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        std::string file_path = folder_path + "/" + entry->d_name;
        unlink(file_path.c_str());
    }
    closedir(dir);
    
    return rmdir(folder_path.c_str()) == 0;
}

std::vector<std::string> TaskManager::getImagePathsInFolder(const std::string& folder_path) {
    std::vector<std::string> paths;
    DIR* dir = opendir(folder_path.c_str());
    if (!dir) return paths;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name = entry->d_name;
        if (name.find(".jpg") != std::string::npos ||
            name.find(".jpeg") != std::string::npos ||
            name.find(".png") != std::string::npos) {
            paths.push_back(folder_path + "/" + name);
        }
    }
    closedir(dir);
    
    // 排序文件名
    std::sort(paths.begin(), paths.end());
    
    return paths;
}
