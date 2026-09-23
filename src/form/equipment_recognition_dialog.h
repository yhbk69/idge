#ifndef EQUIPMENT_RECOGNITION_DIALOG_H
#define EQUIPMENT_RECOGNITION_DIALOG_H

#include <QDialog>
#include <QMetaType>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QStringList>
#include <memory>
#include <map>

#include "../service/equipment_inventory_service.h"

Q_DECLARE_METATYPE(EquipmentTaskResult)

class EquipmentRecognitionDialog : public QDialog {
    Q_OBJECT
public:
    enum Phase { Registration = 0, Cancellation = 1 };

    EquipmentRecognitionDialog(int task_id, const QStringList& paths,
                               std::shared_ptr<EquipmentInventoryService> service,
                               Phase phase, QWidget* parent = nullptr);
    ~EquipmentRecognitionDialog() override;

private slots:
    void startRecognition();
    void onRecognitionFinished(const EquipmentTaskResult& result);
    void onRecognitionError(const QString& message);
    void onPrevious();
    void onCancel();
    void onConfirm();

private:
    void setupUi();
    void renderResults();
    static QString countsText(const std::map<std::string, int>& counts);

    int task_id_;
    QStringList paths_;
    std::shared_ptr<EquipmentInventoryService> service_;
    Phase phase_;
    EquipmentTaskResult result_;
    QThread* recognition_thread_ = nullptr;
    QLabel* status_label_ = nullptr;
    QLabel* total_label_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QTableWidget* table_ = nullptr;
    QPushButton* previous_button_ = nullptr;
    QPushButton* cancel_button_ = nullptr;
    QPushButton* confirm_button_ = nullptr;
};

#endif
