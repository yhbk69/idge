/*
注销识别对话框（文件：cancellation_result_dialog.cpp，实现 CancellationResultDialog）,
处理注销流程
后台线程：调用 service_->matchCancellation() 将注销照片与已登记人脸匹配
匹配表格：
登记人脸图 | 注销人脸图 | 说明（注销匹配/未登记/未注销）| 相似度
用于人工核对哪些人已注销
注销人数计数器：自动统计匹配成功的人数
确认：更新任务状态为已注销，保存匹配记录
*/
// 实现约定：本文件所有中文 UI 文案写作 QStringLiteral("\uXXXX") 转义，
// 规避不同编译器/编辑器对源文件 UTF-8 BOM 处理的差异（防止乱码），并非笔误。
#include "cancellation_result_dialog.h"
#include "photo_selection_widget.h"
#include "../utils/qt_image_utils.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QMessageBox>
#include <QApplication>
#include <QHeaderView>
#include <QTimer>
#include <QMetaType>

Q_DECLARE_METATYPE(CancellationProcessResult)

namespace {
// 匹配表头像单元格：固定 240x190、IgnoreAspectRatio 拉伸填满（登记/注销两列
// 头像来自同尺寸人脸裁剪，拉伸不失真）；loadPixmapSafe 兜底解码失败
QWidget* imageCell(const std::string& path, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFixedSize(240, 190);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("background: #262636; border: 1px solid #45455c; border-radius: 8px;"));
    const QPixmap pixmap = loadPixmapSafe(QString::fromUtf8(path.c_str()));
    if (!pixmap.isNull())
        label->setPixmap(pixmap.scaled(label->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    else
        label->setText(path.empty() ? QStringLiteral("-") : QStringLiteral("Unable to load"));
    return label;
}

/**
 * @class CancellationWorker
 * @brief 注销匹配执行体：在工作线程同步跑 matchCancellation（SCRFD 检测 +
 *        特征比对，占用 NPU），finished/error 跨线程回报，不触碰控件。
 * 路径用 toUtf8() 转字节序；异常统一拦截转成消息，不穿越线程边界。
 * service_ 持 shared_ptr，保证对话框提前销毁时服务对象存活到 run() 结束。
 */
class CancellationWorker : public QObject {
    Q_OBJECT
public:
    CancellationWorker(int task_id, QStringList paths, std::shared_ptr<RollCallService> service)
        : task_id_(task_id), paths_(std::move(paths)), service_(std::move(service)) {}

public slots:
    void run() {
        try {
            std::vector<std::string> paths;
            paths.reserve(paths_.size());
            for (const auto& path : paths_)
                paths.push_back(path.toUtf8().toStdString());
            emit finished(service_->matchCancellation(task_id_, paths));
        } catch (const std::exception& e) {
            emit error(QString::fromUtf8(e.what()));
        } catch (...) {
            emit error(QStringLiteral("Unknown recognition error"));
        }
    }

signals:
    void finished(const CancellationProcessResult& result);
    void error(const QString& message);

private:
    int task_id_;
    QStringList paths_;
    std::shared_ptr<RollCallService> service_;
};
}

// 构造：qRegisterMetaType 供跨线程队列连接搬运 CancellationProcessResult；
// 界面一次性搭好（表格初始禁用、上一步/确认初始禁用），
// singleShot(0) 延迟到事件循环后启动识别，先呈现"正在识别中"+忙碌条
CancellationResultDialog::CancellationResultDialog(int id, const QStringList& paths,
                                                     std::shared_ptr<RollCallService> service,
                                                     QWidget* parent)
    : QDialog(parent), taskId_(id), paths_(paths), service_(std::move(service)) {
    qRegisterMetaType<CancellationProcessResult>("CancellationProcessResult");
    setWindowTitle(QStringLiteral("\u6ce8\u9500\u8bc6\u522b\u7ed3\u679c"));
    resize(1400, 900);
    setMinimumSize(1000, 700);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    auto* header = new QHBoxLayout;
    status_ = new QLabel(QStringLiteral("\u6b63\u5728\u8bc6\u522b\u4e2d..."), this);
    status_->setStyleSheet(QStringLiteral("color: #4fc3f7; font-size:18px; font-weight: 600;"));
    header->addWidget(status_);
    header->addSpacing(20);
    header->addWidget(new QLabel(QStringLiteral("\u6ce8\u9500\u4eba\u6570:"), this));
    count_ = new QSpinBox(this);
    count_->setRange(0, 10000);
    count_->setMinimumHeight(40);
    count_->setEnabled(false);
    header->addWidget(count_);
    loading_ = new QProgressBar(this);
    loading_->setRange(0, 0);
    loading_->setFixedWidth(180);
    loading_->setTextVisible(false);
    header->addWidget(loading_);
    header->addStretch();
    layout->addLayout(header);

    table_ = new QTableWidget(this);
    table_->setColumnCount(4);
    table_->setHorizontalHeaderLabels({QStringLiteral("\u767b\u8bb0\u4eba\u8138"), QStringLiteral("\u6ce8\u9500\u4eba\u8138"),
                                       QStringLiteral("\u8bf4\u660e"), QStringLiteral("\u76f8\u4f3c\u5ea6")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Fixed);
    table_->horizontalHeader()->setSectionResizeMode(3, QHeaderView::Fixed);
    table_->setColumnWidth(2, 150);
    table_->setColumnWidth(3, 110);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEnabled(false);
    layout->addWidget(table_, 1);

    auto* buttons = new QHBoxLayout;
    previous_btn_ = new QPushButton(QStringLiteral("\u4e0a\u4e00\u6b65"), this);
    cancel_btn_ = new QPushButton(QStringLiteral("\u53d6\u6d88"), this);
    confirm_btn_ = new QPushButton(QStringLiteral("\u786e\u8ba4"), this);
    previous_btn_->setEnabled(false);
    confirm_btn_->setEnabled(false);
    buttons->addWidget(previous_btn_);
    buttons->addStretch();
    buttons->addWidget(cancel_btn_);
    buttons->addWidget(confirm_btn_);
    layout->addLayout(buttons);
    connect(previous_btn_, &QPushButton::clicked, this, &CancellationResultDialog::previous);
    connect(cancel_btn_, &QPushButton::clicked, this, &QDialog::reject);
    connect(confirm_btn_, &QPushButton::clicked, this, &CancellationResultDialog::confirm);

    QTimer::singleShot(0, this, &CancellationResultDialog::startRecognition);
}

CancellationResultDialog::~CancellationResultDialog() {
    // quit()+wait() 兜底：matchCancellation 不可中断，关窗时 GUI 等待
    // 识别线程自然结束（已知取舍：极端情况下有数秒卡顿）
    if (recognition_thread_ && recognition_thread_->isRunning()) {
        recognition_thread_->quit();
        recognition_thread_->wait();
    }
}

// Worker→QThread 标准模式（与 RecognitionResultDialog 相同）：
// worker.moveToThread 后由 started 触发 run()；finished/error 让线程 quit，
// thread finished 时 deleteLater worker；worker→dialog 信号跨线程自动队列连接
void CancellationResultDialog::startRecognition() {
    if (!service_) {
        onRecognitionError(QStringLiteral("Recognition service is not initialized"));
        return;
    }
    recognition_thread_ = new QThread(this);
    auto* worker = new CancellationWorker(taskId_, paths_, service_);
    worker->moveToThread(recognition_thread_);
    connect(recognition_thread_, &QThread::started, worker, &CancellationWorker::run);
    connect(worker, &CancellationWorker::finished, this, &CancellationResultDialog::onRecognitionFinished);
    connect(worker, &CancellationWorker::error, this, &CancellationResultDialog::onRecognitionError);
    connect(worker, &CancellationWorker::finished, recognition_thread_, &QThread::quit);
    connect(worker, &CancellationWorker::error, recognition_thread_, &QThread::quit);
    connect(recognition_thread_, &QThread::finished, worker, &QObject::deleteLater);
    recognition_thread_->start();
}

void CancellationResultDialog::onRecognitionFinished(const CancellationProcessResult& result) {
    result_ = result;
    recognize();
    loading_->setVisible(false);
    status_->setText(QStringLiteral("\u8bc6\u522b\u5b8c\u6210"));
    status_->setStyleSheet(QStringLiteral("color: #4caf50; font-size:18px; font-weight: 600;"));
    table_->setEnabled(true);
    count_->setEnabled(true);
    previous_btn_->setEnabled(true);
    confirm_btn_->setEnabled(true);
}

void CancellationResultDialog::onRecognitionError(const QString& message) {
    loading_->setVisible(false);
    status_->setText(QStringLiteral("\u8bc6\u522b\u5931\u8d25"));
    QMessageBox::critical(this, QStringLiteral("\u9519\u8bef"), message);
    previous_btn_->setEnabled(true);
}

// 渲染匹配结果表。match.status 语义（与 RollCallService 约定一致）：
//   1=注销匹配（显示相似度，保留4位小数） 2=未登记 其他=未注销
// 相似度列仅 status==1 有意义；"注销人数"计数器自动取 status==1 的行数
void CancellationResultDialog::recognize() {
    int matched = 0;
    table_->setRowCount(0);
    for (const auto& match : result_.matches) {
        const int row = table_->rowCount();
        table_->insertRow(row);
        table_->setCellWidget(row, 0, imageCell(match.registration_image, table_));
        table_->setCellWidget(row, 1, imageCell(match.cancellation_image, table_));
        table_->setItem(row, 2, new QTableWidgetItem(
            match.status == 1 ? QStringLiteral("\u6ce8\u9500\u5339\u914d") :
            match.status == 2 ? QStringLiteral("\u672a\u767b\u8bb0") : QStringLiteral("\u672a\u6ce8\u9500")));
        table_->setItem(row, 3, new QTableWidgetItem(
            match.status == 1 ? QString::number(match.similarity, 'f', 4) : QStringLiteral("-")));
        table_->setRowHeight(row, 210);
        if (match.status == 1) ++matched;
    }
    count_->setValue(matched);
}

// "上一步"在同一实例内重新选照并再次启动匹配：
// ⚠ startRecognition() 会 new 第二条 QThread(this) 覆盖旧指针——旧线程此时
// 已 finished，仅作为本对话框的子对象滞留到析构，不删除也不悬挂，属已知取舍。
// 重置逻辑：清空表格、禁用结果区、恢复忙碌条后重跑匹配。
void CancellationResultDialog::previous() {
    PhotoSelectionDialog selection(taskId_, service_, {}, this);
    if (selection.exec() == QDialog::Accepted) {
        paths_ = selection.getSelectedPhotos();
        table_->setRowCount(0);
        table_->setEnabled(false);
        previous_btn_->setEnabled(false);
        confirm_btn_->setEnabled(false);
        loading_->setVisible(true);
        status_->setText(QStringLiteral("\u6b63\u5728\u8bc6\u522b\u4e2d..."));
        startRecognition();
    }
}

// 确认注销：以用户可微调的 count_ 为准更新任务注销人数，并落库全部匹配记录；
// 服务层失败（DB/文件异常）时弹错并保持对话框，不静默丢数据
void CancellationResultDialog::confirm() {
    if (service_ && service_->confirmCancellation(taskId_, count_->value(), result_))
        accept();
    else
        QMessageBox::critical(this, QStringLiteral("\u9519\u8bef"), QStringLiteral("\u6ce8\u9500\u786e\u8ba4\u5931\u8d25"));
}

// CancellationWorker 的 Q_OBJECT 元对象代码必须由尾部 moc include 提供，
// 删除本行会导致 vtable 链接错误
#include "cancellation_result_dialog.moc"
