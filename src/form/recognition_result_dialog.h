#ifndef RECOGNITION_RESULT_DIALOG_H
#define RECOGNITION_RESULT_DIALOG_H

#include <QDialog>
#include <QTableWidget>
#include <QLabel>
#include <QSpinBox>
#include <QPushButton>
#include <QProgressBar>
#include <QThread>
#include <QVBoxLayout>
#include <QScrollArea>
#include <memory>
#include <set>
#include <vector>
#include "../service/roll_call_service.h"

Q_DECLARE_METATYPE(TaskProcessResult)

class RecognitionResultDialog : public QDialog {
    Q_OBJECT

public:
    explicit RecognitionResultDialog(int task_id, 
                                    const QStringList& photo_paths,
                                    std::shared_ptr<RollCallService> service,
                                    QWidget* parent = nullptr);
    ~RecognitionResultDialog();

signals:
    void taskConfirmed(int task_id);

private slots:
    void onPrevious();
    void onCancel();
    void onConfirm();
    void onEditTotalCount();
    void onRecognitionFinished(const TaskProcessResult& result);
    void onRecognitionError(const QString& message);

private:
    void setupUI();
    void startRecognition();
    void displayResults();
    void drawFaceBoxes();
    QPixmap drawBoxesOnImage(const QString& image_path, 
                            const std::vector<ProcessedFace>& faces,
                            bool is_first_image,
                            const std::set<int>& duplicate_indices);
    
    int task_id_;
    QStringList photo_paths_;
    std::shared_ptr<RollCallService> service_;
    
    TaskProcessResult result_;
    int total_unique_count_;
    bool is_editing_count_;
    
    // UI组件
    QLabel* total_count_label_;
    QProgressBar* loading_bar_;
    QThread* recognition_thread_;
    QSpinBox* count_spinbox_;
    QPushButton* edit_count_btn_;
    QTableWidget* result_table_;
    QPushButton* prev_btn_;
    QPushButton* cancel_btn_;
    QPushButton* confirm_btn_;
};

#endif // RECOGNITION_RESULT_DIALOG_H
