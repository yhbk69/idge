#include "equipment_recognition_dialog.h"

#include <QAbstractItemView>
#include <QHeaderView>
#include <QMessageBox>
#include <QPixmap>
#include <QTimer>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFileInfo>

#include <utility>

#include "../utils/qt_image_utils.h"

namespace {
class EquipmentRecognitionWorker : public QObject {
    Q_OBJECT
public:
    EquipmentRecognitionWorker(int task_id, QStringList paths,
                               std::shared_ptr<EquipmentInventoryService> service,
                               int phase)
        : task_id_(task_id), paths_(std::move(paths)), service_(std::move(service)), phase_(phase) {}

public slots:
    void run() {
        try {
            std::vector<std::string> source_paths;
            source_paths.reserve(paths_.size());
            for (const auto& path : paths_)
                source_paths.push_back(path.toUtf8().toStdString());
            qInfo() << "[equipment-ui] worker start task=" << task_id_ << "phase=" << phase_
                    << "paths=" << paths_;
            emit finished(service_->processPhotos(task_id_, source_paths, phase_));
        } catch (const std::exception& error) {
            emit failed(QString::fromUtf8(error.what()));
        } catch (...) {
            emit failed(QStringLiteral("设备识别发生未知错误"));
        }
    }

signals:
    void finished(const EquipmentTaskResult& result);
    void failed(const QString& message);

private:
    int task_id_;
    QStringList paths_;
    std::shared_ptr<EquipmentInventoryService> service_;
    int phase_;
};
}

EquipmentRecognitionDialog::EquipmentRecognitionDialog(
    int task_id, const QStringList& paths,
    std::shared_ptr<EquipmentInventoryService> service,
    Phase phase, QWidget* parent)
    : QDialog(parent), task_id_(task_id), paths_(paths),
      service_(std::move(service)), phase_(phase) {
    qRegisterMetaType<EquipmentTaskResult>("EquipmentTaskResult");
    setupUi();
    QTimer::singleShot(0, this, &EquipmentRecognitionDialog::startRecognition);
}

EquipmentRecognitionDialog::~EquipmentRecognitionDialog() {
    if (recognition_thread_ && recognition_thread_->isRunning()) {
        recognition_thread_->quit();
        recognition_thread_->wait();
    }
}

void EquipmentRecognitionDialog::setupUi() {
    setWindowTitle(phase_ == Registration ? QStringLiteral("设备盘点识别结果")
                                          : QStringLiteral("设备注销识别结果"));
    resize(1400, 900);
    setMinimumSize(1000, 700);
    setStyleSheet(QStringLiteral("QDialog { background: #05070C; color: #E5E7EB; }"));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    auto* header = new QHBoxLayout;
    status_label_ = new QLabel(QStringLiteral("正在识别中..."), this);
    status_label_->setStyleSheet(QStringLiteral("color:#38BDF8; font-size:18px; font-weight:600;"));
    header->addWidget(status_label_);
    header->addSpacing(24);
    total_label_ = new QLabel(QStringLiteral("标签总数：0"), this);
    total_label_->setStyleSheet(QStringLiteral("color:#6EE7B7; font-size:20px; font-weight:700;"));
    header->addWidget(total_label_);
    progress_ = new QProgressBar(this);
    progress_->setRange(0, 0);
    progress_->setFixedWidth(180);
    progress_->setTextVisible(false);
    header->addWidget(progress_);
    header->addStretch();
    root->addLayout(header);

    table_ = new QTableWidget(this);
    table_->setColumnCount(2);
    table_->setHorizontalHeaderLabels({QStringLiteral("后处理图片"), QStringLiteral("标签数量")});
    table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    table_->setColumnWidth(1, 280);
    table_->verticalHeader()->setVisible(false);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->setSelectionMode(QAbstractItemView::NoSelection);
    table_->setEnabled(false);
    root->addWidget(table_, 1);

    auto* buttons = new QHBoxLayout;
    previous_button_ = new QPushButton(QStringLiteral("上一步"), this);
    cancel_button_ = new QPushButton(QStringLiteral("取消"), this);
    confirm_button_ = new QPushButton(QStringLiteral("确认"), this);
    previous_button_->setEnabled(false);
    confirm_button_->setEnabled(false);
    buttons->addWidget(previous_button_);
    buttons->addStretch();
    buttons->addWidget(cancel_button_);
    buttons->addWidget(confirm_button_);
    root->addLayout(buttons);
    connect(previous_button_, &QPushButton::clicked, this, &EquipmentRecognitionDialog::onPrevious);
    connect(cancel_button_, &QPushButton::clicked, this, &EquipmentRecognitionDialog::onCancel);
    connect(confirm_button_, &QPushButton::clicked, this, &EquipmentRecognitionDialog::onConfirm);
}

void EquipmentRecognitionDialog::startRecognition() {
    if (!service_) {
        onRecognitionError(QStringLiteral("设备识别服务未初始化"));
        return;
    }
    recognition_thread_ = new QThread(this);
    auto* worker = new EquipmentRecognitionWorker(task_id_, paths_, service_, phase_);
    worker->moveToThread(recognition_thread_);
    connect(recognition_thread_, &QThread::started, worker, &EquipmentRecognitionWorker::run);
    connect(worker, &EquipmentRecognitionWorker::finished,
            this, &EquipmentRecognitionDialog::onRecognitionFinished);
    connect(worker, &EquipmentRecognitionWorker::failed,
            this, &EquipmentRecognitionDialog::onRecognitionError);
    connect(worker, &EquipmentRecognitionWorker::finished,
            recognition_thread_, &QThread::quit);
    connect(worker, &EquipmentRecognitionWorker::failed,
            recognition_thread_, &QThread::quit);
    connect(recognition_thread_, &QThread::finished, worker, &QObject::deleteLater);
    recognition_thread_->start();
}

QString EquipmentRecognitionDialog::countsText(const std::map<std::string, int>& counts) {
    if (counts.empty()) return QStringLiteral("未检测到目标");
    QStringList parts;
    for (const auto& item : counts)
        parts << QStringLiteral("%1: %2").arg(QString::fromUtf8(item.first.c_str())).arg(item.second);
    return parts.join(QStringLiteral("\n"));
}

void EquipmentRecognitionDialog::onRecognitionFinished(const EquipmentTaskResult& result) {
    qInfo() << "[equipment-ui] finished success=" << result.success
            << "photos=" << result.photos.size()
            << "error=" << QString::fromUtf8(result.error_message.c_str());
    if (!result.success) {
        onRecognitionError(QString::fromUtf8(result.error_message.c_str()));
        return;
    }
    result_ = result;
    renderResults();
    status_label_->setText(QStringLiteral("识别完成"));
    status_label_->setStyleSheet(QStringLiteral("color:#6EE7B7; font-size:18px; font-weight:600;"));
    total_label_->setText(QStringLiteral("标签总数\n%1").arg(countsText(result_.total_counts)));
    progress_->setVisible(false);
    table_->setEnabled(true);
    previous_button_->setEnabled(true);
    confirm_button_->setEnabled(true);
}

void EquipmentRecognitionDialog::onRecognitionError(const QString& message) {
    progress_->setVisible(false);
    status_label_->setText(QStringLiteral("识别失败"));
    QMessageBox::critical(this, QStringLiteral("设备识别失败"), message);
    previous_button_->setEnabled(true);
}

void EquipmentRecognitionDialog::renderResults() {
    table_->setRowCount(0);
    for (const auto& photo : result_.photos) {
        const QString path = QString::fromUtf8(photo.processed_path.c_str());
        qInfo() << "[equipment-ui] render processed=" << path
                << "exists=" << QFileInfo(path).exists() << "size=" << QFileInfo(path).size();
        const int row = table_->rowCount();
        table_->insertRow(row);
        auto* image = new QLabel(table_);
        image->setMinimumSize(0, 320);
        image->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        image->setAlignment(Qt::AlignCenter);
        image->setStyleSheet(QStringLiteral("background:#0A0D12; border:1px solid #1E2636;"));
        const QPixmap pixmap = loadPixmapSafe(path);
        if (!pixmap.isNull())
            image->setPixmap(pixmap.scaled(760, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            image->setText(QStringLiteral("图片无法加载"));
        table_->setCellWidget(row, 0, image);

        auto* counts = new QLabel(countsText(photo.counts), table_);
        counts->setAlignment(Qt::AlignCenter);
        counts->setStyleSheet(QStringLiteral("color:#E5E7EB; font-size:18px; font-weight:600;"));
        counts->setMinimumWidth(220);
        table_->setCellWidget(row, 1, counts);
        table_->setRowHeight(row, 350);
    }
}

void EquipmentRecognitionDialog::onPrevious() { reject(); }

void EquipmentRecognitionDialog::onCancel() {
    if (QMessageBox::question(this, QStringLiteral("确认取消"),
                              QStringLiteral("确定取消当前设备任务吗？")) == QMessageBox::Yes)
        done(2);
}

void EquipmentRecognitionDialog::onConfirm() {
    if (!service_->saveResult(result_)) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("保存设备盘点结果失败"));
        return;
    }
    accept();
}

#include "equipment_recognition_dialog.moc"
