/* test_equipment —— 设备盘点全链离线自测（无人值守板端验收）
 *
 * 覆盖链路：RollCallService 初始化(共库) -> 盘点服务模型加载(进程内 YOLO11Model)
 *          -> 建任务 -> processPhotos 检测计数 -> 画框图落盘 -> saveResult 落库
 *          -> getPhotos/getDetections 回读一致性。
 * 数据全部写在 /tmp 临时工作区，不碰正式 data/roll_call_data。
 * 用法：仓库根目录执行 ./build/build_rk3588_linux/test_equipment
 * 退出码：0=PASS，1=FAIL（stdout 给出失败环节）。
 */
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QString>

#include <iostream>
#include <string>
#include <vector>

#include "equipment_inventory_service.h"
#include "model_registry.h"
#include "roll_call_service.h"

namespace {

int g_failures = 0;

void check(bool condition, const std::string& what) {
    if (condition) {
        std::cout << "  [ok]   " << what << std::endl;
    } else {
        std::cout << "  [FAIL] " << what << std::endl;
        ++g_failures;
    }
}

} // namespace

int main(int argc, char** argv) {
    // 画框链走 loadPixmapSafe(QPixmap)，Qt 要求 QGuiApplication 先于任何 QPixmap 构造；
    // offscreen 平台使自测无需 DISPLAY
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QGuiApplication app(argc, argv);

    const QString workspace = QDir::currentPath();
    const QString test_image = workspace + "/assets/test/bus.jpg";
    const QString face_dir = workspace + "/model/face";
    std::cout << "[test_equipment] workspace=" << workspace.toStdString() << std::endl;

    // ModelRegistry 单例的自动扫描挂在 frmmain 页构建路径上，离线自测需显式 rescan
    ModelRegistry::instance().rescan();
    if (!QFileInfo::exists(test_image) || !ModelRegistry::instance().contains("yolo11n-coco")) {
        std::cout << "  [FAIL] 前置资源缺失：需在仓库根运行（assets/test/bus.jpg + model/library/yolo11n-coco）" << std::endl;
        return 1;
    }

    // 临时工作区：db 与任务目录都进 /tmp，跑完即删
    const QString tmp_root = QStringLiteral("/tmp/equip_check_%1").arg(QCoreApplication::applicationPid());
    QDir().mkpath(tmp_root);
    std::cout << "[test_equipment] tmp workspace=" << tmp_root.toStdString() << std::endl;

    // 1. 点名服务（盘点共库依赖）：两件套权重同主程序约定（进程内识别器）
    auto roll_call = std::make_shared<RollCallService>();
    check(roll_call->initialize(
              (face_dir + "/detection.rknn").toStdString(),
              (face_dir + "/recognition.rknn").toStdString(),
              (tmp_root + "/roll_call.db").toStdString(),
              (tmp_root + "/roll_call_data").toStdString()),
          "RollCallService::initialize");

    // 2. 盘点服务：模型清单与 frmMain::equipmentModelConfigsFromLibrary 同口径
    auto inventory = std::make_shared<EquipmentInventoryService>(roll_call);
    ModelRegistry& reg = ModelRegistry::instance();
    const std::string coco_id = "yolo11n-coco";
    check(inventory->initialize({{"",
              reg.modelPath(QString::fromStdString(coco_id)).toStdString(),
              reg.labelPath(QString::fromStdString(coco_id)).toStdString()}}),
          "EquipmentInventoryService::initialize (yolo11n-coco, 进程内)");
    if (g_failures) { std::cout << "FAIL: 服务初始化未通过，终止" << std::endl; QDir(tmp_root).removeRecursively(); return 1; }

    // 3. 建任务 -> 批量检测（bus.jpg：COCO 类预期 bus/person 等计数>0）
    const int task_id = inventory->createEquipmentTask("离线自测任务");
    check(task_id > 0, "createEquipmentTask");
    const auto result = inventory->processPhotos(
        task_id, {test_image.toUtf8().toStdString()}, /*phase=*/0);
    check(result.success, "processPhotos success");
    int total = 0;
    for (const auto& item : result.total_counts) {
        std::cout << "    count: " << item.first << " = " << item.second << std::endl;
        total += item.second;
    }
    check(total > 0, "盘点总计数 > 0");
    if (!result.error_message.empty())
        std::cout << "    error_message: " << result.error_message << std::endl;
    if (!result.photos.empty()) {
        const auto& photo = result.photos.front();
        check(!photo.processed_path.empty() &&
              QFileInfo(QString::fromUtf8(photo.processed_path.c_str())).size() > 0,
              "processed_*.png 画框图生成且非空");
    }

    // 4. 落库 + 回读一致性
    check(inventory->saveResult(result), "saveResult");
    const auto photos_in_db = inventory->getPhotos(task_id, 0);
    check(photos_in_db.size() == result.photos.size(), "getPhotos 回读张数一致");
    if (!photos_in_db.empty()) {
        const auto detections_in_db = inventory->getDetections(photos_in_db.front().id);
        check(!detections_in_db.empty() &&
              detections_in_db.size() == result.photos.front().detections.size(),
              "getDetections 回读明细数一致");
    }

    // 5. 热更新原子性（frmMain::reloadEquipmentService 的语义底座）：
    //    合法新清单 -> 换模型成功；非法清单 -> 返回 false 且旧模型完整可用
    const std::string coco_model = reg.modelPath(QString::fromStdString(coco_id)).toStdString();
    const std::string coco_labels = reg.labelPath(QString::fromStdString(coco_id)).toStdString();
    check(inventory->initialize({{"", coco_model, coco_labels},
                                {"", coco_model, coco_labels}}) &&
              inventory->modelConfigs().size() == 2,
          "热更新：双模型清单加载成功");
    const bool ready_before = inventory->isReady();
    check(!inventory->initialize({{"", "/nonexistent/model.rknn", "/nonexistent/labels.txt"}}) &&
              ready_before && inventory->isReady() &&
              inventory->modelConfigs().size() == 2,
          "热更新：非法清单失败后保持原模型（原子性）");
    const auto rerun = inventory->processPhotos(
        task_id, {test_image.toUtf8().toStdString()}, /*phase=*/0);
    check(rerun.success && !rerun.photos.empty() && !rerun.photos.front().detections.empty(),
          "热更新失败后旧模型仍可完成检测");

    QDir(tmp_root).removeRecursively();
    if (g_failures == 0) { std::cout << "PASS" << std::endl; return 0; }
    std::cout << "FAIL: " << g_failures << " 项未通过" << std::endl;
    return 1;
}
