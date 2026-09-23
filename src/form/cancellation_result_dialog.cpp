/*
注销识别对话框,处理注销流程
后台线程：调用 service_->matchCancellation() 将注销照片与已登记人脸匹配
匹配表格：
登记人脸图 | 注销人脸图 | 说明（注销匹配/未登记/未注销）| 相似度
用于人工核对哪些人已注销
注销人数计数器：自动统计匹配成功的人数
确认：更新任务状态为已注销，保存匹配记录
*/
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
QWidget* imageCell(const std::string& path, QWidget* parent) {
    auto* label = new QLabel(parent);
    label->setFixedSize(240, 190);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("background: #0A0D12; border: 1px solid #1E2636; border-radius: 8px;"));
    const QPixmap pixmap = loadPixmapSafe(QString::fromUtf8(path.c_str()));
    if (!pixmap.isNull())
        label->setPixmap(pixmap.scaled(label->size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation));
    else
        label->setText(path.empty() ? QStringLiteral("-") : QStringLiteral("Unable to load"));
    return label;
}

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
    status_->setStyleSheet(QStringLiteral("color: #38BDF8; font-size: 16px; font-weight: 600;"));
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
    if (recognition_thread_ && recognition_thread_->isRunning()) {
        recognition_thread_->quit();
        recognition_thread_->wait();
    }
}

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
    status_->setStyleSheet(QStringLiteral("color: #6EE7B7; font-size: 16px; font-weight: 600;"));
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

void CancellationResultDialog::confirm() {
    if (service_ && service_->confirmCancellation(taskId_, count_->value(), result_))
        accept();
    else
        QMessageBox::critical(this, QStringLiteral("\u9519\u8bef"), QStringLiteral("\u6ce8\u9500\u786e\u8ba4\u5931\u8d25"));
}

#include "cancellation_result_dialog.moc"
