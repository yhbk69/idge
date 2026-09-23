#pragma once
#include <QDialog>
#include <QTableWidget>
#include <QSpinBox>
#include <QProgressBar>
#include <QLabel>
#include <QThread>
#include <QPushButton>
#include <QStringList>
#include <memory>
#include "../service/roll_call_service.h"
class CancellationResultDialog : public QDialog {
 Q_OBJECT
public: CancellationResultDialog(int taskId,const QStringList& paths,std::shared_ptr<RollCallService> service,QWidget* p=nullptr);
    ~CancellationResultDialog() override;
private slots:
    void confirm();
    void previous();
    void startRecognition();
    void onRecognitionFinished(const CancellationProcessResult& result);
    void onRecognitionError(const QString& message);

private:
    void recognize();
    int taskId_;
    QStringList paths_;
    std::shared_ptr<RollCallService> service_;
    CancellationProcessResult result_;
    QTableWidget* table_ = nullptr;
    QSpinBox* count_ = nullptr;
    QProgressBar* loading_ = nullptr;
    QLabel* status_ = nullptr;
    QPushButton* previous_btn_ = nullptr;
    QPushButton* cancel_btn_ = nullptr;
    QPushButton* confirm_btn_ = nullptr;
    QThread* recognition_thread_ = nullptr;
};
