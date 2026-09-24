#include "delegate.h"
#include "qcombobox.h"
#include "qdebug.h"

// ============================================================================
// 构造函数：保存下拉框选项列表
// ============================================================================
DelegateComboBox::DelegateComboBox(const QStringList &delegateValue, QObject *parent) : QStyledItemDelegate(parent)
{
    this->delegateValue = delegateValue;
}

// ============================================================================
// createEditor - 创建QComboBox编辑器
// 在单元格被双击时自动调用，创建下拉框供用户选择
// ============================================================================
QWidget *DelegateComboBox::createEditor(QWidget *parent, const QStyleOptionViewItem &option, const QModelIndex &index) const
{
    QComboBox *cbox = new QComboBox(parent);
    cbox->addItems(delegateValue);  // 填充预设选项
    return cbox;
}

// ============================================================================
// setEditorData - 从模型读取数据到编辑器
// 将单元格当前显示的文本设置为QComboBox的选中项
// ============================================================================
void DelegateComboBox::setEditorData(QWidget *editor, const QModelIndex &index) const
{
    QString data = index.data(Qt::DisplayRole).toString();
    QComboBox *cbox = static_cast<QComboBox *>(editor);
    cbox->setCurrentIndex(cbox->findText(data));  // 根据文本查找并设置选中项
}

// ============================================================================
// setModelData - 将编辑器数据写回模型
// 用户选择完成后，将QComboBox的选中值写入模型
// ============================================================================
void DelegateComboBox::setModelData(QWidget *editor, QAbstractItemModel *model, const QModelIndex &index) const
{
    QComboBox *cbox = static_cast<QComboBox *>(editor);
    QString data = cbox->currentText();
    model->setData(index, data);
}
