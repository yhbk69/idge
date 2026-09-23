// 文件职责：任务目录/文件管理器声明，负责创建带 _N 后缀去重的任务文件夹、
// 生成"时间戳+随机数"唯一文件名及扩展名规范化，为点名/盘点任务提供磁盘产物组织规则。
#ifndef TASK_MANAGER_H
#define TASK_MANAGER_H

#include <string>
#include <vector>

class TaskManager {
public:
    // 创建任务文件夹，返回文件夹绝对路径
    // base_path: 基础存储路径（如 "./roll_call_data"）
    // task_name: 任务名称
    static std::string createTaskFolder(const std::string& base_path, const std::string& task_name);
    
    // 生成唯一文件名（时间戳 + 随机数）
    // prefix: 前缀（如 "original", "processed", "face"）
    // extension: 扩展名（如 ".jpg"）
    static std::string generateUniqueFilename(const std::string& prefix, const std::string& extension);
    
    // 保存图片到任务文件夹
    // folder_path: 任务文件夹路径
    // image: 图片数据
    // filename: 文件名
    static std::string saveImageToFolder(const std::string& folder_path, 
                                         const void* image_data, 
                                         int width, int height, int channels,
                                         const std::string& filename);
    
    // 删除任务文件夹及所有内容
    static bool deleteTaskFolder(const std::string& folder_path);
    
    // 获取任务文件夹中所有图片路径
    static std::vector<std::string> getImagePathsInFolder(const std::string& folder_path);
};

#endif // TASK_MANAGER_H