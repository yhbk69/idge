/* 蔡超添加
任务管理工具
主要用于人员点名和设备盘点模块
负责创建任务文件夹、生成唯一文件名、保存图片到指定目录 
*/
/**
 * @file task_manager.cpp
 * @brief 点名/盘点任务的磁盘产物组织规则（目录命名、唯一文件名、抓拍落盘、清理）
 *
 * 所属模块：src/biz/service（见同目录 README.md）。全部是**静态方法 + 无状态**，
 * 只用 POSIX + OpenCV 写图，不依赖 Qt、不依赖数据库，因此点名与盘点两条链路共用一份规则。
 * 这里定下来的是"产物长什么样"：任务目录 `<base>/<任务名>[_N]`、
 * 文件名 `<prefix>_<毫秒时间戳>_<4 位随机数>.<ext>`，DB 里存的就是这两者拼出来的路径，
 * 改任何一条命名规则都会让历史任务的图片显示不出来（库里存的是路径文本，不是相对键）。
 *
 * 线程与进程：本文件自身不加锁。调用方（RollCallService / EquipmentInventoryService）
 * 内部是单线程串行批处理，所以"同名任务并发创建""同一毫秒生成同名文件"在当前用法下
 * 不会发生；换到并发场景要先看 createTaskFolder 与 generateUniqueFilename 上的 NOTE。
 * 失败一律用返回值表达（空串 / false），不抛异常、不打日志——调用方自己决定怎么提示用户。
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

/**
 * @brief 建任务目录，重名自动加 `_N` 后缀
 * @param base_path 存储根（如 "roll_call_data"）；不存在时先 mkdir 一层，
 *                  返回值被忽略——多级不存在/只读时留给下面的 mkdir 报错，这里不额外处理
 * @param task_name 用户输入的任务名，允许中文（ext4 上的 UTF-8 目录名合法）
 * @return 创建成功的**拼接路径**（不是 realpath，base_path 给相对就是相对返回相对）；失败返回空串
 *
 * 字符替换只动 空格/冒号/斜杠 三种：反斜杠、glob 元字符（* ? [）、以 `-` 开头等都原样进
 * 目录名。NOTE(未处理): 这些在 Linux 上都能建成合法目录，只有当路径被再拼进 shell 命令
 * 或用作通配时才会出问题——当前没有这种用法（删除走 unlink/rmdir，不经过 shell）。
 *
 * 0755 是刻意权限：产物目录要被同机的其它工具（日志抓取/人工查看）读，不需要写。
 * TODO(并发): 重名判定用 access() 后再 mkdir()，两步之间有竞态；同机同时开两个实例
 * 建同名任务可能双双拿到同一个路径、后建的写进同一目录。当前不存在该场景。
 */
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

/**
 * @brief 生成"前缀_毫秒时间戳_4 位随机数.扩展名"形式的唯一文件名
 * @param extension 允许带或不带点（历史上两种写法都传过，这里统一补点，见函数内注释）；
 *                  传空串会得到 "." 结尾的名字——调用方必须给真扩展名
 * @return 仅文件名（不含目录），拼到 folder_path 后面才是完整路径
 *
 * 唯一性的两个来源：毫秒时间戳（同一秒内的不同照片基本靠它分开）+ rand()%10000。
 * 本文件**不播种**：进程里由 QtHelper::initRand()（src/ui/core/qthelper.cpp）按时间设种子，
 * 所以主程序跑起来时随机数是真的；离线单测直接调本类时没经过那条初始化，rand() 序列
 * 每次启动都一样——靠毫秒时间戳仍然不重名，但别把这里当"随机数发生器"用。
 * NOTE(风险): 同一毫秒内连续两次取名的话，后缀只是 0..9999 的 rand()，有 1/1e4 概率撞名，
 * 撞名后果是后写的图覆盖前一张（cv::imwrite 静默覆写）。当前调用方都是一张一张串行取名，
 * 没这个场景；要并发改成 atomic 计数后缀，别只调 rand()。
 * 时间戳是墙钟：NTP 回拨理论上会让不同真实时刻得到同一个数。
 */
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

/**
 * @brief 把调用方持有的像素缓冲按 cv::Mat 头写盘
 * @param image_data 非拥有指针：**必须**是紧凑排列（step == width*channels）且 8UC1/8UC3；
 *        带 padding 的缓冲（如某些解码器输出）会被当成错位像素，图会斜/花而不是报错
 * @param channels 只认 1 与 3；4（BGRA/RGBA）直接返回空串，**不会**自动转 3 通道
 * @return 写入成功的完整路径；任何失败（类型不支持 / imwrite 失败）返回空串且无日志
 *
 * .clone() 是必须的：cv::Mat 包住外部指针时不拷贝也不拥有，而 cv::imwrite 之前
 * 调用方就可能把那块缓冲释放掉（抓拍缓冲是上层的临时对象）。
 * NOTE(与本目录其它写图路径不一致): roll_call_service 的落盘刻意走 QImage::save 而不是
 * cv::imwrite（理由见 src/biz/service/roll_call_service.cpp 里的注释——板端 OpenCV 编码后端
 * 会崩），本函数仍是 imwrite。改这里之前先确认是不是也该一起换，别留下两套编码后端。
 */
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

/**
 * @brief 删任务目录：只删**一层**普通文件，然后 rmdir
 *
 * ⚠ 不递归：目录里若还有子目录，unlink() 对它返回失败（被忽略），最后 rmdir() 因目录非空
 * 失败并返回 false——现象是"点了删除但任务目录还在"。当前点名/盘点的产物都是平铺的
 * 图片，所以够用；将来要放子目录（如按日期再分一层），这里得改成递归删。
 * 单个文件 unlink 失败也不上报（继续删后面的），所以返回值只说明"目录删掉了没有"。
 * 不经过 shell、不碰数据库——tasks 行的删除是调用方（业务服务）另做的事，
 * 服务 README 里的顺序约定是"先删文件夹后删库"。
 */
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
