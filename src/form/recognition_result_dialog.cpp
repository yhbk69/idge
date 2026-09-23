/*
识别结果对话框（文件：recognition_result_dialog.cpp，实现 RecognitionResultDialog），
处理登记照片的人脸识别：
后台线程：调用 service_->processPhotos() 进行 NPU 人脸检测和特征提取
结果表格：
第一列：后处理图片（绘制人脸框，重复人脸用黄色虚线框标记）
第二列：不重复人数（大号数字）
编辑人数：可手动修改总人数（切换 spinbox）
上一步：返回照片选择界面重新拍照
确认：保存结果到数据库，写入人脸特征和图片路径
*/
#include "../utils/qt_image_utils.h"
#include "recognition_result_dialog.h"
#include <QHeaderView>
#include <QMessageBox>
#include <QProgressDialog>
#include <QFileInfo>
#include <QDebug>
#include <QApplication>
#include <QTimer>
#include <QDateTime>
#include <QPainter>
#include <QPen>
#include <set>
#include <QMetaType>

/**
 * @class RecognitionWorker
 * @brief 登记识别执行体：在工作线程同步跑 processPhotos()
 *        （NPU 人脸检测 + 特征提取 + 跨照片去重 + 后处理图落盘），
 *        仅经 finished/error 信号回报，不触碰任何控件。
 * @note 持 service_ 的 shared_ptr：即使对话框先销毁，服务对象也存活到
 *       run() 结束，识别线程不会持有悬垂引用。异常全部在此拦截转消息。
 */
class RecognitionWorker : public QObject {
    Q_OBJECT
public:
    RecognitionWorker(int task_id, QStringList paths, std::shared_ptr<RollCallService> service)
        : task_id_(task_id), paths_(std::move(paths)), service_(std::move(service)) {}
public slots:
    void run() {
        try {
            std::vector<std::string> paths;
            for (const auto& path : paths_) paths.push_back(path.toStdString());
            emit finished(service_->processPhotos(task_id_, paths));
        } catch (const std::exception& e) { emit error(QString::fromUtf8(e.what())); }
          catch (...) { emit error(QStringLiteral("未知异常")); }
    }
signals:
    void finished(const TaskProcessResult& result);
    void error(const QString& message);
private:
    int task_id_; QStringList paths_; std::shared_ptr<RollCallService> service_;
};

// 构造：先注册元类型（跨线程信号携带 TaskProcessResult 的前提），
// 再搭 UI；识别用 singleShot(0) 推迟到事件循环后启动
RecognitionResultDialog::RecognitionResultDialog(int task_id,
                                                 const QStringList& photo_paths,
                                                 std::shared_ptr<RollCallService> service,
                                                 QWidget* parent)
    : QDialog(parent),
      task_id_(task_id),
      photo_paths_(photo_paths),
      service_(service),
      total_unique_count_(0),
      is_editing_count_(false),
      loading_bar_(nullptr), recognition_thread_(nullptr) {
    qRegisterMetaType<TaskProcessResult>("TaskProcessResult");
    setupUI();
    // Defer recognition until the dialog has entered the event loop. This
    // lets the result window paint its loading state before NPU processing.
    QTimer::singleShot(0, this, &RecognitionResultDialog::startRecognition);
}

RecognitionResultDialog::~RecognitionResultDialog() {
    // quit()+wait()：processPhotos 不可中断，识别中关窗会等待其自然结束
    // （已知取舍：极端情况下 GUI 有数秒卡顿，勿改为 detach/terminate）
    if (recognition_thread_ && recognition_thread_->isRunning()) {
        recognition_thread_->quit();
        recognition_thread_->wait();
    }
}

void RecognitionResultDialog::setupUI() {
    setWindowTitle("识别结果");
    resize(1400, 900);
    setStyleSheet("QDialog { background: #05070C; }");
    
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(24, 24, 24, 24);
    main_layout->setSpacing(20);
    
    // 标题和总人
    auto* header_layout = new QHBoxLayout();
    
    auto* title = new QLabel("人脸识别结果", this);
    QFont title_font;
    title_font.setPointSize(20);
    title_font.setBold(true);
    title->setFont(title_font);
    title->setStyleSheet("color: #E5E7EB;");
    header_layout->addWidget(title);
    
    header_layout->addStretch();
    
    total_count_label_ = new QLabel("总人数", this);
    QFont count_font;
    count_font.setPointSize(24);
    count_font.setBold(true);
    total_count_label_->setFont(count_font);
    total_count_label_->setStyleSheet("color: #6EE7B7; padding: 12px 24px; background: #0F131C; border-radius: 12px;");
    header_layout->addWidget(total_count_label_);

    loading_bar_ = new QProgressBar(this);
    loading_bar_->setRange(0, 0);
    loading_bar_->setFixedWidth(180);
    loading_bar_->setTextVisible(false);
    loading_bar_->setStyleSheet("QProgressBar { background: #0F131C; border: 1px solid #1E2636; border-radius: 5px; height: 10px; } QProgressBar::chunk { background: #38BDF8; }");
    loading_bar_->setVisible(false);
    header_layout->addWidget(loading_bar_);
    
    count_spinbox_ = new QSpinBox(this);
    count_spinbox_->setMinimum(0);
    count_spinbox_->setMaximum(10000);
    count_spinbox_->setMinimumHeight(48);
    count_spinbox_->setVisible(false);
    count_spinbox_->setStyleSheet(
        "QSpinBox {"
        "  background: #0F131C;"
        "  color: #E5E7EB;"
        "  border: 2px solid #38BDF8;"
        "  border-radius: 8px;"
        "  padding: 8px 16px;"
        "  font-size: 18px;"
        "  font-weight: 700;"
        "}"
    );
    header_layout->addWidget(count_spinbox_);
    
    edit_count_btn_ = new QPushButton("编辑人数", this);
    edit_count_btn_->setMinimumSize(120, 48);
    edit_count_btn_->setStyleSheet(
        "QPushButton {"
        "  background: #1E2636;"
        "  color: #E9A568;"
        "  border: none;"
        "  border-radius: 999px;"
        "  padding: 0 24px;"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: #161D2B; }"
    );
    connect(edit_count_btn_, &QPushButton::clicked, this, &RecognitionResultDialog::onEditTotalCount);
    header_layout->addWidget(edit_count_btn_);
    
    main_layout->addLayout(header_layout);
    
    // 结果表格
    result_table_ = new QTableWidget(this);
    result_table_->setColumnCount(2);
    result_table_->setHorizontalHeaderLabels({"后处理图", "不重复人数"});
    result_table_->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    result_table_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Fixed);
    result_table_->setColumnWidth(1, 260);
    result_table_->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft);
    result_table_->verticalHeader()->setVisible(false);
    result_table_->setSelectionMode(QAbstractItemView::NoSelection);
    result_table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    result_table_->setShowGrid(false);
    result_table_->verticalHeader()->setDefaultSectionSize(400);
    result_table_->setStyleSheet(
        "QTableWidget {"
        "  background: #0A0D12;"
        "  border: 2px solid #161D2B;"
        "  border-radius: 12px;"
        "  gridline-color: #161D2B;"
        "  color: #E5E7EB;"
        "}"
        "QTableWidget::item {"
        "  padding: 16px;"
        "  border-bottom: 1px solid #161D2B;"
        "}"
        "QHeaderView::section {"
        "  background: #0F131C;"
        "  color: #9CA3AF;"
        "  padding: 16px;"
        "  border: none;"
        "  border-bottom: 2px solid #38BDF8;"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "}"
    );
    main_layout->addWidget(result_table_, 1);
    
    // 底部按钮
    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->setSpacing(12);
    bottom_layout->addStretch();
    
    prev_btn_ = new QPushButton("上一步", this);
    prev_btn_->setMinimumSize(120, 48);
    prev_btn_->setStyleSheet(
        "QPushButton {"
        "  background: #1E2636;"
        "  color: #E5E7EB;"
        "  border: none;"
        "  border-radius: 999px;"
        "  padding: 0 24px;"
        "  font-size: 15px;"
        "  font-weight: 600;"
        "}"
        "QPushButton:hover { background: #161D2B; }"
    );
    connect(prev_btn_, &QPushButton::clicked, this, &RecognitionResultDialog::onPrevious);
    bottom_layout->addWidget(prev_btn_);
    
    cancel_btn_ = new QPushButton("取消", this);
    cancel_btn_->setMinimumSize(120, 48);
    cancel_btn_->setStyleSheet(prev_btn_->styleSheet());
    connect(cancel_btn_, &QPushButton::clicked, this, &RecognitionResultDialog::onCancel);
    bottom_layout->addWidget(cancel_btn_);
    
    confirm_btn_ = new QPushButton("确认", this);
    confirm_btn_->setMinimumSize(140, 48);
    confirm_btn_->setStyleSheet(
        "QPushButton {"
        "  background: #6EE7B7;"
        "  color: #05070C;"
        "  border: none;"
        "  border-radius: 999px;"
        "  padding: 0 32px;"
        "  font-size: 16px;"
        "  font-weight: 700;"
        "}"
        "QPushButton:hover { background: #34D399; }"
        "QPushButton:pressed { background: #10B981; }"
    );
    connect(confirm_btn_, &QPushButton::clicked, this, &RecognitionResultDialog::onConfirm);
    bottom_layout->addWidget(confirm_btn_);
    
    main_layout->addLayout(bottom_layout);
}

/**
 * @brief 启动识别后台线程（Worker→QThread 标准模式）
 *
 * 顺序：new QThread(this) → worker.moveToThread → started 触发 run()，
 * finished/error 连回本对话框槽（跨线程自动队列连接，槽内操作控件安全），
 * 同时连到线程 quit；线程 finished 后 deleteLater worker，闭环无泄漏。
 * 启动前把表格/按钮全部禁用并显示忙碌条，防止识别中用户重复操作。
 */
void RecognitionResultDialog::startRecognition() {
    if (!service_) {
        QMessageBox::critical(this, QString::fromUtf8("错误"), QString::fromUtf8("点名服务未初始化"));
        reject();
        return;
    }
    // Paint the result dialog first so the user sees a loading state while
    // the synchronous NPU process runs.
    total_count_label_->setText("正在识别...");
    loading_bar_->setVisible(true);
    result_table_->setEnabled(false);
    prev_btn_->setEnabled(false);
    cancel_btn_->setEnabled(false);
    confirm_btn_->setEnabled(false);
    recognition_thread_ = new QThread(this);
    auto* worker = new RecognitionWorker(task_id_, photo_paths_, service_);
    worker->moveToThread(recognition_thread_);
    connect(recognition_thread_, &QThread::started, worker, &RecognitionWorker::run);
    connect(worker, &RecognitionWorker::finished, this, &RecognitionResultDialog::onRecognitionFinished);
    connect(worker, &RecognitionWorker::error, this, &RecognitionResultDialog::onRecognitionError);
    connect(worker, &RecognitionWorker::finished, recognition_thread_, &QThread::quit);
    connect(worker, &RecognitionWorker::error, recognition_thread_, &QThread::quit);
    connect(recognition_thread_, &QThread::finished, worker, &QObject::deleteLater);
    recognition_thread_->start();
}

void RecognitionResultDialog::onRecognitionFinished(const TaskProcessResult& result) {
    result_ = result;
    total_count_label_->setText(QString("总人数 %1").arg(result_.total_unique_count));
    
    // 计算总人数
    total_unique_count_ = result_.total_unique_count;
    total_count_label_->setText(QString("总人数 %1").arg(total_unique_count_));
    loading_bar_->setVisible(false);
    result_table_->setEnabled(true);
    prev_btn_->setEnabled(true);
    cancel_btn_->setEnabled(true);
    confirm_btn_->setEnabled(true);
    
    // 显示结果
    displayResults();
}

void RecognitionResultDialog::onRecognitionError(const QString& message) {
    loading_bar_->setVisible(false);
    total_count_label_->setText("识别失败");
    prev_btn_->setEnabled(true); cancel_btn_->setEnabled(true);
    QMessageBox::critical(this, "识别失败", message);
}

// 逐张照片填充结果表：第一列后处理图（服务层已把人脸框画在图上，
// UI 只做 760x350 等比缩放展示，行高 400 与之配套；解码失败退化为
// "已识别 N 张人脸"文字占位），第二列为大号(48pt)不重复人数。
// 注意板端 Qt JPEG 插件与图像流水线所用 libjpeg ABI 不兼容，
// 必须走 loadPixmapSafe，不可改回 QPixmap 直接构造。
void RecognitionResultDialog::displayResults() {
    qDebug() << "[rollcall-ui] displayResults begin photos=" << result_.photos.size();
    result_table_->setRowCount(0);
    
    for (size_t i = 0; i < result_.photos.size(); ++i) {
        const auto& photo = result_.photos[i];
        
        qDebug() << "[rollcall-ui] display photo" << i << "faces=" << photo.faces.size();
        int row = result_table_->rowCount();
        result_table_->insertRow(row);
        
        // 第一列：后处理图片（带人脸框
        auto* image_widget = new QWidget();
        auto* image_layout = new QVBoxLayout(image_widget);
        image_layout->setContentsMargins(12, 12, 12, 12);
        
        // 找出重复人脸的索
        // ⚠ 遗留：duplicate_indices 现无任何消费者（原供 drawBoxesOnImage 使用），
        // 属死代码计算，随 UI 侧画框方案废弃而失去作用，保留仅为对齐历史逻辑
        std::set<int> duplicate_indices;
        for (size_t j = 0; j < photo.faces.size(); ++j) {
            if (photo.faces[j].is_duplicate) {
                duplicate_indices.insert(j);
            }
        }
        
        // 绘制人脸
        auto* image_label = new QLabel(image_widget);
        // The board's Qt JPEG plugin is ABI-incompatible with libjpeg used by
        // the image pipeline. Avoid decoding JPEG here; recognition data is
        // still fully available and saved below.
        QPixmap processed_pixmap = loadPixmapSafe(QString::fromStdString(photo.processed_path));
        if (!processed_pixmap.isNull())
            image_label->setPixmap(processed_pixmap.scaled(760, 350, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        else
            image_label->setText(QString("已识别%1 张人脸").arg(photo.faces.size()));
        image_label->setAlignment(Qt::AlignCenter);
        image_label->setStyleSheet(
            "QLabel {"
            "  background: #0F131C;"
            "  border: 2px solid #161D2B;"
            "  border-radius: 8px;"
            "  padding: 8px;"
            "}"
        );
        image_layout->addWidget(image_label);
        
        // 文件
        auto* filename_label = new QLabel(QFileInfo(QString::fromStdString(photo.original_path)).fileName(), image_widget);
        filename_label->setAlignment(Qt::AlignCenter);
        filename_label->setStyleSheet(
            "color: #9CA3AF;"
            "font-size: 13px;"
            "margin-top: 8px;"
        );
        image_layout->addWidget(filename_label);
        
        result_table_->setCellWidget(row, 0, image_widget);
        
        // 第二列：不重复人数
        auto* count_widget = new QWidget();
        auto* count_layout = new QVBoxLayout(count_widget);
        count_layout->setAlignment(Qt::AlignCenter);
        
        auto* count_label = new QLabel(QString::number(photo.unique_count), count_widget);
        QFont count_font;
        count_font.setPointSize(48);
        count_font.setBold(true);
        count_label->setFont(count_font);
        count_label->setAlignment(Qt::AlignCenter);
        count_label->setStyleSheet("color: #38BDF8;");
        count_layout->addWidget(count_label);
        
        result_table_->setCellWidget(row, 1, count_widget);
    }
}

/**
 * @brief 在图片上二次绘制人脸框（绿色实线=不重复，橙黄虚线=重复，线宽 8）
 * @warning ⚠ 遗留死代码警示：全工程无调用点。后处理图的人脸框已由
 *          RollCallService 在落盘 processed_path 时绘制完成，UI 侧重绘方案
 *          已废弃（同时规避板端 JPEG 解码限制）。清理前勿当作缺失功能重接；
 *          配套声明 drawFaceBoxes() 更是只声明未实现（调用即链接失败）。
 */
QPixmap RecognitionResultDialog::drawBoxesOnImage(const QString& image_path,
                                                  const std::vector<ProcessedFace>& faces,
                                                  bool is_first_image,
                                                  const std::set<int>& duplicate_indices) {
    QPixmap pixmap = loadPixmapSafe(image_path);
    if (pixmap.isNull()) {
        return QPixmap();
    }
    
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    
    for (size_t i = 0; i < faces.size(); ++i) {
        const auto& face = faces[i];
        
        // 确定颜色和样
        QColor box_color;
        Qt::PenStyle pen_style;
        int pen_width;
        
        if (is_first_image) {
            // 第一张图：所有框都是绿色实线
            box_color = QColor(110, 231, 183);  // #6EE7B7
            pen_style = Qt::SolidLine;
            pen_width = 8;
        } else {
            if (duplicate_indices.find(i) != duplicate_indices.end()) {
                // 重复人脸：黄色虚
                box_color = QColor(233, 165, 104);  // #E9A568
                pen_style = Qt::DashLine;
                pen_width = 8;
            } else {
                // 不重复人脸：绿色实线
                box_color = QColor(110, 231, 183);  // #6EE7B7
                pen_style = Qt::SolidLine;
                pen_width = 8;
            }
        }
        
        QPen pen(box_color, pen_width, pen_style);
        painter.setPen(pen);
        
        // 绘制矩形
        // ProcessedFace stores the detection box as cv::Rect (x, y, width, height).
        const cv::Rect &r = face.rect;
        painter.drawRect(QRectF(r.x, r.y, r.width, r.height));
        
        // 绘制人脸ID标签（可选）
        if (!is_first_image && duplicate_indices.find(i) == duplicate_indices.end()) {
            painter.fillRect(QRectF(r.x, r.y - 30, 60, 30), box_color);
            painter.setPen(QColor(5, 7, 12));  // #05070C
            QFont font;
            font.setPointSize(12);
            font.setBold(true);
            painter.setFont(font);
            painter.drawText(QRectF(r.x, r.y - 30, 60, 30), 
                           Qt::AlignCenter, 
                           QString("ID:%1").arg(face.similar_to_index + 1));
        }
    }
    
    painter.end();
    return pixmap;
}

void RecognitionResultDialog::onEditTotalCount() {
    // 就地编辑：总人数 label 与 spinbox(0~10000) 互斥显隐，
    // 保存时写回 total_unique_count_ 与 result_，确认落库以人工值为准
    if (is_editing_count_) {
        // 保存编辑
        total_unique_count_ = count_spinbox_->value();
        total_count_label_->setText(QString("总人数 %1").arg(total_unique_count_));
        total_count_label_->setVisible(true);
        count_spinbox_->setVisible(false);
        edit_count_btn_->setText("编辑人数");
        is_editing_count_ = false;
        
        result_.total_unique_count = total_unique_count_;
    } else {
        // 开始编
        count_spinbox_->setValue(total_unique_count_);
        total_count_label_->setVisible(false);
        count_spinbox_->setVisible(true);
        edit_count_btn_->setText("保存");
        is_editing_count_ = true;
    }
}

void RecognitionResultDialog::onPrevious() {
    reject();  // 返回照片选择界面
}

void RecognitionResultDialog::onCancel() {
    // 取消=删任务：确认后由服务层连库带文件删除，done(2) 为自定义返回码，
    // 外层 RollCallWidget 识别到 2 只提示"任务已取消"，不再重复 deleteTask
    auto reply = QMessageBox::question(
        this, "确认取消",
        "确定要取消整个任务吗？所有数据将被删除",
        QMessageBox::Yes | QMessageBox::No
    );
    
    if (reply == QMessageBox::Yes) {
        // 删除任务
        if (service_->deleteTask(task_id_)) {
            done(2);  // 自定义返回码表示取消任务
        }
    }
}

void RecognitionResultDialog::onConfirm() {
    // 保存结果（含人工修正后的 total_unique_count、人脸特征、图片路径）；
    // 失败保持对话框可重试，成功才 accept 并广播 taskConfirmed
    // （taskConfirmed 目前全工程无接收者，仅保留扩展点，勿依赖它驱动刷新）
    // 保存结果
    if (!service_->saveTaskResult(result_)) {
        QMessageBox::critical(this, "错误", "保存失败");
        return;
    }
    
    QMessageBox::information(this, "成功", 
        QString("任务已保存，共登记%1 人").arg(total_unique_count_));
    
    emit taskConfirmed(task_id_);
    accept();
}

// RecognitionWorker 定义在本 .cpp 且带 Q_OBJECT，尾部必须包含 AUTOMOC
// 生成的元对象实现，否则 vtable/静态元对象符号链接失败（勿删除）
#include "recognition_result_dialog.moc"



