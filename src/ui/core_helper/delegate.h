#ifndef DELEGATE_H
#define DELEGATE_H

// ============================================================================
// 自定义委托类头文件
// 提供QComboBox下拉框作为表格/列表的编辑器
// 基于QStyledItemDelegate实现，支持自定义编辑控件
// ============================================================================

#include <QStyledItemDelegate>

// ============================================================================
// DelegateComboBox - 下拉框委托类
// 在QTableView/QListView等视图中使用下拉框进行编辑
// ============================================================================
class DelegateComboBox : public QStyledItemDelegate
{
    Q_OBJECT

public:
    /**
     * @brief 构造函数
     * @param delegateValue [in] 下拉框的选项列表
     * @param parent [in] 父对象
     */
    explicit DelegateComboBox(const QStringList &delegateValue, QObject *parent = 0);

protected:
    /**
     * @brief 创建编辑器控件
     * 返回一个填充了预设选项的QComboBox
     */
    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const;

    /**
     * @brief 设置编辑器数据
     * 将模型中的当前值设置到QComboBox
     */
    void setEditorData(QWidget *editor, const QModelIndex &index) const;

    /**
     * @brief 将编辑器数据写回模型
     * 从QComboBox获取选中值并写入模型
     */
    void setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const;

private:
    QStringList delegateValue;  // 下拉框选项列表
};

#endif // DELEGATE_H
