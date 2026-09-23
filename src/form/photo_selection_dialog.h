#ifndef PHOTO_SELECTION_DIALOG_H
#define PHOTO_SELECTION_DIALOG_H

#include <QDialog>
#include <QLabel>
#include <QScrollArea>
#include <QGridLayout>
#include <QPushButton>
#include <QStringList>
#include <QCheckBox>
#include <memory>
#include <opencv2/opencv.hpp>
#include "../service/roll_call_service.h"

class PhotoSelectionDialog : public QDialog {
    Q_OBJECT

public:
    explicit PhotoSelectionDialog(int task_id, std::shared_ptr<RollCallService> service, QWidget* parent = nullptr);
    ~PhotoSelectionDialog();

    QStringList getSelectedPhotos() const { return selected_photos_; }

signals:
    void photosConfirmed(const QStringList& photos);

private slots:
    void onTakePhoto();
    void onUploadPhotos();
    void onEditPhotos();
    void onDeleteSelected();
    void onCancelEdit();
    void onNext();
    void onCancel();

private:
    void setupUI();
    void renderPhotoPreviews();
    void showCameraDialog();
    void enterEditMode();
    void exitEditMode();

    int task_id_;
    std::shared_ptr<RollCallService> service_;
    
    QStringList selected_photos_;
    bool is_edit_mode_;
    
    // UI缁勪欢
    QScrollArea* scroll_area_;
    QWidget* photo_container_;
    QGridLayout* photo_layout_;
    
    QPushButton* take_photo_btn_;
    QPushButton* upload_btn_;
    QPushButton* edit_btn_;
    QPushButton* delete_btn_;
    QPushButton* cancel_edit_btn_;
    QPushButton* next_btn_;
    QPushButton* cancel_btn_;
    
    std::vector<QCheckBox*> photo_checkboxes_;
};

#endif // PHOTO_SELECTION_DIALOG_H
