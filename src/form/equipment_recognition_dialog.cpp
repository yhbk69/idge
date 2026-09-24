// 文件：equipment_recognition_dialog.cpp
// 职责：设备盘点识别结果对话框实现（caichao 分支合入），
//       后台 QThread + Worker(moveToThread) 执行 NPU 设备检测，结果回 GUI 线程渲染
#include "equipment_recognition_dialog.h"
#include "theme.h"

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
/**
 * @class EquipmentRecognitionWorker
 * @brief 识别任务执行体：整段 processPhotos 在工作线程同步跑（含 NPU 推理与落盘），
 *        仅通过 finished/failed 信号跨线程汇报，不触碰任何 Qt 控件。
 *
 * 路径转换用 toUtf8()（板端文件系统路径按 UTF-8 字节序处理）；
 * 所有 C++ 异常在此拦截转为 QString 消息——绝不允许多态异常穿越线程边界。
 * 注意 Worker 持有 service_ 的 shared_ptr：即使对话框提前销毁，
 * 服务对象也会被 Worker 续命到 run() 结束，避免悬垂引用。
 */
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
    // 用 singleShot(0) 把识别推迟到事件循环启动后：
    // 先让 exec() 绘制出"正在识别中"+忙碌进度条，再进入耗时流程
    QTimer::singleShot(0, this, &EquipmentRecognitionDialog::startRecognition);
}

EquipmentRecognitionDialog::~EquipmentRecognitionDialog() {
    // quit() 只结束工作线程的事件循环，无法打断正在执行的 processPhotos；
    // wait() 会阻塞 GUI 直到识别自然结束——关窗表现为短暂无响应，属已知行为
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
    setStyleSheet(QString("QDialog { background: %1; color: %2; }").arg(theme::BG, theme::TEXT));

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(24, 24, 24, 24);
    root->setSpacing(16);

    auto* header = new QHBoxLayout;
    status_label_ = new QLabel(QStringLiteral("正在识别中..."), this);
    status_label_->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:600;")
                                     .arg(theme::ACCENT)
                                     .arg(theme::FS_CARD));
    header->addWidget(status_label_);
    header->addSpacing(24);
    total_label_ = new QLabel(QStringLiteral("标签总数：0"), this);
    total_label_->setStyleSheet(theme::text(theme::SUCCESS, theme::FS_PAGE, true));
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

/**
 * @brief 启动识别后台线程（标准 Worker→QThread 模式）
 *
 * 线程安全要点：
 *   - QThread 以 this 为 parent，随对话框析构统一回收；本对话框每个实例
 *     只启动一条识别线程（"上一步"走 reject 后由调用方重建整个对话框）；
 *   - worker 不 setParent，所有权交给线程：finished/failed → quit，
 *     thread finished → worker deleteLater，闭环无泄漏；
 *   - worker→dialog 的信号跨线程，Qt 自动按队列连接投递到 GUI 线程，
 *     槽内可安全操作控件。
 */
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
    status_label_->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:600;")
                                     .arg(theme::SUCCESS)
                                     .arg(theme::FS_CARD));
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

// 结果表逐行渲染：后处理图等比缩到 760x320（与表格行高 350 匹配），
// 第二列固定宽 280px 放"标签: 数量"多行文本；图片同步解码，
// loadPixmapSafe 失败时显示占位文字而不是崩溃
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
        image->setStyleSheet(QString("background:%1; border:1px solid %2;").arg(theme::FIELD, theme::HOVER));
        const QPixmap pixmap = loadPixmapSafe(path);
        if (!pixmap.isNull())
            image->setPixmap(pixmap.scaled(760, 320, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            image->setText(QStringLiteral("图片无法加载"));
        table_->setCellWidget(row, 0, image);

        auto* counts = new QLabel(countsText(photo.counts), table_);
        counts->setAlignment(Qt::AlignCenter);
        counts->setStyleSheet(QString("color:%1; font-size:%2px; font-weight:600;")
                                  .arg(theme::TEXT)
                                  .arg(theme::FS_CARD));
        counts->setMinimumWidth(220);
        table_->setCellWidget(row, 1, counts);
        table_->setRowHeight(row, 350);
    }
}

void EquipmentRecognitionDialog::onPrevious() { reject(); }  // Accepted 之外的标准码：调用方回到选照步骤

void EquipmentRecognitionDialog::onCancel() {
    // done(2) 为自定义结果码（既非 Accepted 也非 Rejected）：
    // 登记流程中调用方据此把任务整体删除；注销流程仅关闭窗口
    if (QMessageBox::question(this, QStringLiteral("确认取消"),
                              QStringLiteral("确定取消当前设备任务吗？")) == QMessageBox::Yes)
        done(2);
}

void EquipmentRecognitionDialog::onConfirm() {
    // 只有 saveResult 落库成功才 accept()；失败保留界面让用户重试/取消
    if (!service_->saveResult(result_)) {
        QMessageBox::critical(this, QStringLiteral("错误"), QStringLiteral("保存设备盘点结果失败"));
        return;
    }
    accept();
}

// EquipmentRecognitionWorker 带 Q_OBJECT 且定义在本 .cpp 内，
// 必须尾部包含该 moc 产物，否则 vtable 链接失败（AUTOMOC 生成）
#include "equipment_recognition_dialog.moc"
