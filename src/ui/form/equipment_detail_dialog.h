#ifndef EQUIPMENT_DETAIL_DIALOG_H
#define EQUIPMENT_DETAIL_DIALOG_H

/**
 * @file equipment_detail_dialog.h
 * @brief 设备盘点任务详情对话框 —— caichao 分支合入
 *
 * 职责：以模态 QDialog 展示单个盘点任务的登记/注销后处理图片、
 *       每张照片的标签计数与总计，只读快照，不触发识别。
 */

#include <QDialog>
#include <map>
#include <memory>

#include "equipment_inventory_service.h"

/**
 * @class EquipmentDetailDialog
 * @brief 盘点详情页。构造即 setupUi()+loadData() 完成同步取数与建表，
 *        调用方以 exec() 模态使用，栈上对象关闭即析构。
 *
 * 注意：imageWidget/photoCell 里对每张照片都要 getDetections() 一次
 * （N+1 查询），且图片经 loadPixmapSafe 同步解码——照片数量多时打开
 * 会短暂阻塞 GUI 线程，属已知取舍（板端照片量小）。
 * 遗留警示：photoCell() 当前无任何调用点（loadData 已改为直接内联
 * imageWidget+counts 组合），属死代码，改动前请确认无外部依赖。
 */
class EquipmentDetailDialog : public QDialog {
    Q_OBJECT
public:
    EquipmentDetailDialog(int task_id,
                          std::shared_ptr<EquipmentInventoryService> service,
                          QWidget* parent = nullptr);

private:
    // 把检测记录/计数 map 渲染成 "标签: 数量" 多行文本（两重载共用核心逻辑）
    static QString countsText(const std::vector<EquipmentDetectionRecord>& detections);
    static QString countsText(const std::map<std::string, int>& counts);
    static QWidget* imageWidget(const std::string& path, QWidget* parent);  ///< 只读缩略图单元（420x260 缩放）
    QWidget* photoCell(const EquipmentPhotoRecord& photo, QWidget* parent) const; ///< 图片+标签计数纵向组合（遗留：当前未被调用）
    void setupUi();   ///< 只设窗口属性，布局在 loadData() 中按数据动态构建
    void loadData();  ///< 查询任务/照片/检测记录并组装表格（登记 2 列，已注销 4 列）

    int task_id_;
    std::shared_ptr<EquipmentInventoryService> service_;  ///< 由调用方保证非空（页面注入的服务）
};

#endif
