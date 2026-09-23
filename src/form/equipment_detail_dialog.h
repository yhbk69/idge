#ifndef EQUIPMENT_DETAIL_DIALOG_H
#define EQUIPMENT_DETAIL_DIALOG_H

#include <QDialog>
#include <map>
#include <memory>

#include "../service/equipment_inventory_service.h"

class EquipmentDetailDialog : public QDialog {
    Q_OBJECT
public:
    EquipmentDetailDialog(int task_id,
                          std::shared_ptr<EquipmentInventoryService> service,
                          QWidget* parent = nullptr);

private:
    static QString countsText(const std::vector<EquipmentDetectionRecord>& detections);
    static QString countsText(const std::map<std::string, int>& counts);
    static QWidget* imageWidget(const std::string& path, QWidget* parent);
    QWidget* photoCell(const EquipmentPhotoRecord& photo, QWidget* parent) const;
    void setupUi();
    void loadData();

    int task_id_;
    std::shared_ptr<EquipmentInventoryService> service_;
};

#endif
