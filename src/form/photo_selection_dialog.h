#ifndef PHOTO_SELECTION_DIALOG_H
#define PHOTO_SELECTION_DIALOG_H

/**
 * @file photo_selection_dialog.h
 * @brief 【遗留未使用】旧版照片选择对话框声明（caichao 分支早期版本）
 *
 * 状态警示：
 *   - 全工程无任何 #include 引用本头文件，也无对应 .cpp 实现，未参与构建；
 *   - 其功能已被 photo_selection_widget.h/.cpp 中同名类 PhotoSelectionDialog
 *     取代（旧版为 3 列网格 + 编辑模式 + 复选框删除；新版为纵向列表 +
 *     常驻 × 按钮）。两文件声明了同名类，绝不可同时包含，否则重定义；
 *   - 保留目的：历史交互参考。删除前请再次确认可 grep 到零引用。
 * 注释修正说明：原文件内部分中文注释因编码损坏呈乱码（如 "UI缁勪欢"），
 * 现按原义恢复为 "UI组件" 等可读中文，仅改注释不改声明。
 */

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

/**
 * @class PhotoSelectionDialog (遗留版本声明)
 * @brief 旧版选照界面：网格照片预览(renderPhotoPreviews) + 编辑模式
 *        (enterEditMode/exitEditMode 切换复选框删除)，无相机预览实现体。
 */
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
    
    // UI组件（原注释为乱码 "UI缁勪欢"，系 GBK/UTF-8 编码损坏，已修正）
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
