#include "stream_editor_dialog.h"

#include "theme.h"
#include "qthelper.h"   // enableTouchScroll：运行期才建的对话框，构造末尾的全量扫描扫不到它

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

// ============================================================
// 视频流注册表编辑（video.sources[]）
// ============================================================
// 为什么要有这个对话框：把"通道里手填地址"改成"通道引用流 id"之后，
// 现场需要一个能看见并维护这张表的地方。只靠手改 JSON 的话，
// 四路通道 + 智能识别共用的那只摄像头就会各写各的地址（改一处漏三处）。
//
// 一条流的 id 在这里是**只读**的：改名等于换一条流，所有引用它的通道都会变成
// 悬空引用（解析不出来 → 那一路直接不开）。与其在界面上做"改名 + 顺手改所有引用"
// 这套容易漏的联动，不如让新建/删除成为唯一的入口。
// ============================================================

namespace {

// 类型档：与 ConfigManager 里 type 的取值一一对应（"mp4" 会被归一成 "file"，
// 所以界面上不再单独给一档，避免同一个东西出现两种写法）
const char *const kTypes[] = {"file", "camera", "rtsp", "rtmp"};
// 摄像头输入格式档。空串 = 不指定，交给解码循环里的三档协商（nv12→yuyv→mjpeg）
const char *const kInputFormats[] = {"", "nv12", "yuyv", "mjpeg"};

QString streamListItemText(const VideoSourceCfg &s)
{
    // 列表里就要能看出"这条流是什么"，否则现场要在四条同名条目里来回点才知道选的是哪个
    return QStringLiteral("%1  ·  %2  ·  %3")
        .arg(s.id, s.type.isEmpty() ? QStringLiteral("(自动判型)") : s.type,
             s.note.isEmpty() ? s.url : QStringLiteral("%1 (%2)").arg(s.note, s.url));
}

} // namespace

StreamEditorDialog::StreamEditorDialog(QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("视频流管理（video.sources）"));
    // 触屏优先：给足宽高，别让人在四宫格后面摸小按钮
    setMinimumSize(760, 480);

    ConfigManager &cfg = ConfigManager::instance();
    streams_ = cfg.videoSources();

    buildUi();
    // 有流就默认选中第一条并把表单填上；注册表为空时列表没行可选，
    // refreshList 会把右侧整片禁用（只剩"新建流"可点）。
    refreshList();
}

void StreamEditorDialog::buildUi()
{
    auto *root = new QVBoxLayout(this);
    root->setSpacing(10);

    auto *hint = new QLabel(QStringLiteral(
        "同一只摄像头/同一路地址只写一次，通道在下拉里引用它。"));
    hint->setWordWrap(true);
    hint->setStyleSheet(theme::text(theme::TEXT, theme::FS_BODY));
    root->addWidget(hint);

    auto *body = new QHBoxLayout;
    body->setSpacing(12);
    root->addLayout(body, 1);

    // ── 左侧：流列表 + 新建/删除 ─────────────────────────────────
    auto *left = new QVBoxLayout;
    left->setSpacing(6);
    list_ = new QListWidget;
    list_->setMinimumWidth(280);
    list_->setStyleSheet(theme::field(theme::BORDER));
    QtHelper::enableTouchScroll(list_);   // 流一多就得拖着滚，触屏没有滚轮
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem *cur, QListWidgetItem *prev) {
                // 收的是**上一条**的表单值：信号发出来时 cur 已经是新项了，
                // 用 currentId() 去 applyToModel 会把 A 的值写进 B（切一次条，两条一起坏）。
                if (prev) {
                    applyToModel(prev->data(Qt::UserRole).toString());
                }
                if (cur) {
                    loadStream(cur->data(Qt::UserRole).toString());
                }
            });
    left->addWidget(list_, 1);

    auto *listBtns = new QHBoxLayout;
    auto *addBtn = new QPushButton(QStringLiteral("新建流"));
    addBtn->setMinimumHeight(theme::TOUCH_FIELD_H);
    addBtn->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    connect(addBtn, &QPushButton::clicked, this, &StreamEditorDialog::addStream);
    listBtns->addWidget(addBtn);

    auto *delBtn = new QPushButton(QStringLiteral("删除"));
    delBtn->setMinimumHeight(theme::TOUCH_FIELD_H);
    delBtn->setStyleSheet(theme::mainButton(theme::DANGER));
    connect(delBtn, &QPushButton::clicked, this, &StreamEditorDialog::removeStream);
    listBtns->addWidget(delBtn);
    left->addLayout(listBtns);
    body->addLayout(left);

    // ── 右侧：参数表单 ──────────────────────────────────────────
    auto *form = new QFormLayout;
    form->setSpacing(8);
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);

    idEdit_ = new QLineEdit;
    idEdit_->setReadOnly(true);   // 见文件头：改名 = 换一条流，这里不给这个口子
    idEdit_->setMinimumHeight(theme::TOUCH_FIELD_H);
    idEdit_->setStyleSheet(theme::field(theme::BORDER));
    form->addRow(QStringLiteral("流 id"), idEdit_);

    typeCombo_ = new QComboBox;
    typeCombo_->setMinimumHeight(theme::TOUCH_FIELD_H);
    theme::makeComboTouch(typeCombo_);   // 弹层行高：本体够大但弹层仍是鼠标档就点不中
    for (const char *t : kTypes) {
        typeCombo_->addItem(QString::fromLatin1(t), QString::fromLatin1(t));
    }
    typeCombo_->insertItem(0, QStringLiteral("(自动判型)"), QString());
    form->addRow(QStringLiteral("类型"), typeCombo_);

    auto *urlRow = new QHBoxLayout;
    urlRow->setSpacing(6);
    urlEdit_ = new QLineEdit;
    urlEdit_->setPlaceholderText(QStringLiteral("/dev/video41 ｜ /path/to/a.mp4 ｜ rtsp://…"));
    urlEdit_->setMinimumHeight(theme::TOUCH_FIELD_H);
    urlEdit_->setStyleSheet(theme::field(theme::BORDER));
    urlRow->addWidget(urlEdit_, 1);
    browseBtn_ = new QPushButton(QStringLiteral("浏览…"));
    browseBtn_->setMinimumHeight(theme::TOUCH_FIELD_H);
    browseBtn_->setStyleSheet(theme::mainButton(theme::BTN_SECONDARY));
    connect(browseBtn_, &QPushButton::clicked, this, &StreamEditorDialog::browseUrl);
    urlRow->addWidget(browseBtn_);
    form->addRow(QStringLiteral("地址"), urlRow);

    widthSpin_ = new QSpinBox;
    widthSpin_->setRange(0, 8192);
    widthSpin_->setSpecialValueText(QStringLiteral("不指定"));
    widthSpin_->setSuffix(QStringLiteral(" px"));
    widthSpin_->setMinimumHeight(theme::TOUCH_FIELD_H);
    form->addRow(QStringLiteral("宽（仅摄像头）"), widthSpin_);

    heightSpin_ = new QSpinBox;
    heightSpin_->setRange(0, 8192);
    heightSpin_->setSpecialValueText(QStringLiteral("不指定"));
    heightSpin_->setSuffix(QStringLiteral(" px"));
    heightSpin_->setMinimumHeight(theme::TOUCH_FIELD_H);
    form->addRow(QStringLiteral("高（仅摄像头）"), heightSpin_);

    fpsSpin_ = new QSpinBox;
    fpsSpin_->setRange(0, 240);
    fpsSpin_->setSpecialValueText(QStringLiteral("默认 30"));
    fpsSpin_->setSuffix(QStringLiteral(" fps"));
    fpsSpin_->setMinimumHeight(theme::TOUCH_FIELD_H);
    form->addRow(QStringLiteral("帧率（仅摄像头）"), fpsSpin_);

    inputFormatCombo_ = new QComboBox;
    inputFormatCombo_->setMinimumHeight(theme::TOUCH_FIELD_H);
    theme::makeComboTouch(inputFormatCombo_);
    for (const char *f : kInputFormats) {
        inputFormatCombo_->addItem(f ? QString::fromLatin1(f) : QStringLiteral("三档协商"),
                                   QString::fromLatin1(f));
    }
    form->addRow(QStringLiteral("输入格式"), inputFormatCombo_);

    loopCheck_ = new QCheckBox(QStringLiteral("文件播完回到开头（loop）"));
    loopCheck_->setMinimumHeight(theme::TOUCH_FIELD_H);
    form->addRow(QString(), loopCheck_);

    hwCheck_ = new QCheckBox(QStringLiteral("硬件解码（rkmpp；关掉走 CPU 软解）"));
    hwCheck_->setMinimumHeight(theme::TOUCH_FIELD_H);
    hwCheck_->setChecked(true);
    form->addRow(QString(), hwCheck_);

    timeoutSpin_ = new QSpinBox;
    timeoutSpin_->setRange(0, 120000);
    timeoutSpin_->setSingleStep(500);
    timeoutSpin_->setSpecialValueText(QStringLiteral("不限"));
    timeoutSpin_->setSuffix(QStringLiteral(" ms"));
    timeoutSpin_->setMinimumHeight(theme::TOUCH_FIELD_H);
    form->addRow(QStringLiteral("打开超时"), timeoutSpin_);

    reconnectSpin_ = new QSpinBox;
    reconnectSpin_->setRange(0, 120000);
    reconnectSpin_->setSingleStep(500);
    reconnectSpin_->setSpecialValueText(QStringLiteral("不重连"));
    reconnectSpin_->setSuffix(QStringLiteral(" ms"));
    reconnectSpin_->setMinimumHeight(theme::TOUCH_FIELD_H);
    form->addRow(QStringLiteral("断流重连间隔"), reconnectSpin_);

    noteEdit_ = new QLineEdit;
    noteEdit_->setPlaceholderText(QStringLiteral("东门摄像头"));
    noteEdit_->setMinimumHeight(theme::TOUCH_FIELD_H);
    noteEdit_->setStyleSheet(theme::field(theme::BORDER));
    form->addRow(QStringLiteral("备注"), noteEdit_);

    // 右侧包进滚动区：十三行 × 触屏档最小高（theme::TOUCH_FIELD_H）已经高过对话框的
    // 最小高度，不滚动就只能把控件压回非触屏尺寸——那是本目录明确禁止的取舍。
    // 对话框是运行期才构造的，frmMain 构造末尾那次全量扫描（enableTouchScrollAll）
    // 扫不到它，所以两处都要在这里显式挂。
    auto *formScroll = new QScrollArea;
    formScroll->setWidgetResizable(true);
    formScroll->setFrameShape(QFrame::NoFrame);
    formScroll->setMinimumWidth(380);
    auto *formHost = new QWidget;
    formHost->setLayout(form);
    formScroll->setWidget(formHost);
    QtHelper::enableTouchScroll(formScroll);
    body->addWidget(formScroll, 1);

    auto *btns = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(btns, &QDialogButtonBox::accepted, this, &StreamEditorDialog::onAccept);
    connect(btns, &QDialogButtonBox::rejected, this, &QDialog::reject);
    root->addWidget(btns);
}

QString StreamEditorDialog::currentId() const
{
    QListWidgetItem *it = list_ ? list_->currentItem() : nullptr;
    return it ? it->data(Qt::UserRole).toString() : QString();
}

void StreamEditorDialog::refreshList(const QString &selectId)
{
    // 全程屏蔽信号：重建列表 + 复位选中**不是**用户动作。放着信号出去，
    // currentItemChanged 会拿"上一条"去 applyToModel，把还留在表单里的旧值写进
    // 刚选中的那一条——切一次条，两条同时坏。用户真的点列表时信号是通的（见 buildUi）。
    list_->blockSignals(true);
    list_->clear();
    for (const VideoSourceCfg &s : streams_) {
        auto *it = new QListWidgetItem(streamListItemText(s));
        it->setData(Qt::UserRole, s.id);
        list_->addItem(it);
    }
    int row = -1;
    for (int i = 0; i < list_->count(); ++i) {
        if (list_->item(i)->data(Qt::UserRole).toString() == selectId) {
            row = i;
            break;
        }
    }
    if (row < 0 && list_->count() > 0)
        row = 0;   // 要选的那条不在了（新建失败/被删）：退到第一行，右侧不至于空白
    list_->setCurrentRow(row);
    list_->blockSignals(false);
    setFieldEnabled(!streams_.isEmpty());
    if (row >= 0)
        loadStream(list_->item(row)->data(Qt::UserRole).toString());
}

void StreamEditorDialog::setFieldEnabled(bool on)
{
    // 写成 QList<QWidget*> 而不是 auto 初始化列表：花括号里的控件类型不一致
    // （QLineEdit/QComboBox/QSpinBox…），`{a, b}` 让编译器自己推 auto 会当场判成
    // "推不出公共类型"，而 QList<QWidget*> 是每一项各自向上转换。
    const QList<QWidget *> fields = {idEdit_,   typeCombo_,  urlEdit_,   browseBtn_,
                                     widthSpin_, heightSpin_, fpsSpin_,   inputFormatCombo_,
                                     loopCheck_, hwCheck_,    timeoutSpin_, reconnectSpin_,
                                     noteEdit_};
    for (QWidget *w : fields) {
        w->setEnabled(on);
    }
}

void StreamEditorDialog::loadStream(const QString &id)
{
    if (id.isEmpty()) {
        return;
    }
    for (const VideoSourceCfg &s : streams_) {
        if (s.id != id) {
            continue;
        }
        // 这里没连任何编辑信号，纯赋值：回填过程中不会有"把半套表单值写回条目"的机会。
        idEdit_->setText(s.id);
        const int typeIdx = typeCombo_->findData(s.type);
        typeCombo_->setCurrentIndex(typeIdx >= 0 ? typeIdx : 0);
        urlEdit_->setText(s.url);
        widthSpin_->setValue(s.width);
        heightSpin_->setValue(s.height);
        fpsSpin_->setValue(s.fps);
        const int fmtIdx = inputFormatCombo_->findData(s.inputFormat);
        inputFormatCombo_->setCurrentIndex(fmtIdx >= 0 ? fmtIdx : 0);
        loopCheck_->setChecked(s.loop);
        hwCheck_->setChecked(s.hardwareDecode);
        timeoutSpin_->setValue(s.timeoutMs);
        reconnectSpin_->setValue(s.reconnectMs);
        noteEdit_->setText(s.note);
        return;
    }
}

void StreamEditorDialog::applyToModel(const QString &id)
{
    if (id.isEmpty()) {
        return;
    }
    for (VideoSourceCfg &s : streams_) {
        if (s.id != id) {
            continue;
        }
        const VideoSourceCfg before = s;
        s.type = typeCombo_->currentData().toString();
        s.url = urlEdit_->text().trimmed();
        s.width = widthSpin_->value();
        s.height = heightSpin_->value();
        s.fps = fpsSpin_->value();
        s.inputFormat = inputFormatCombo_->currentData().toString();
        s.loop = loopCheck_->isChecked();
        s.hardwareDecode = hwCheck_->isChecked();
        s.timeoutMs = timeoutSpin_->value();
        s.reconnectMs = reconnectSpin_->value();
        s.note = noteEdit_->text().trimmed();
        if (before.url != s.url || before.type != s.type || before.loop != s.loop
                || before.hardwareDecode != s.hardwareDecode || before.width != s.width
                || before.height != s.height || before.fps != s.fps
                || before.inputFormat != s.inputFormat || before.timeoutMs != s.timeoutMs
                || before.reconnectMs != s.reconnectMs) {
            if (!touched_.contains(s.id)) {
                touched_.append(s.id);
            }
        }
        // 列表文字里的 url/备注也要跟着变，否则左侧看上去还是旧值
        for (int i = 0; i < list_->count(); ++i) {
            if (list_->item(i)->data(Qt::UserRole).toString() == s.id) {
                list_->item(i)->setText(streamListItemText(s));
            }
        }
        return;
    }
}

void StreamEditorDialog::addStream()
{
    applyToModel(currentId());   // 先固化正在编辑的那条，新建才不会把半套表单值丢在旧条目上

    // id 自动生成且不重名：手填 id 是这里最容易出错的一步（撞车会让"命中哪条流"
    // 取决于数组顺序），既然 id 只读，那就由程序起名。
    QString id;
    for (int n = 1; n <= 99; ++n) {
        id = QStringLiteral("stream-%1").arg(n, 2, 10, QLatin1Char('0'));
        bool free = true;
        for (const VideoSourceCfg &s : streams_) {
            if (s.id == id) {
                free = false;
                break;
            }
        }
        if (free) {
            break;
        }
    }

    VideoSourceCfg s;
    s.id = id;
    s.type = QStringLiteral("file");
    s.url = QStringLiteral("assets/test/192.mp4");   // 先给一个一定打得开的地址，改它就行
    s.hardwareDecode = true;
    s.loop = true;
    streams_.append(s);
    if (!touched_.contains(id)) {
        touched_.append(id);
    }
    refreshList(id);        // 选中新建的这条并把它的默认值填进表单
    urlEdit_->setFocus();
    urlEdit_->selectAll();
}

void StreamEditorDialog::removeStream()
{
    const QString id = currentId();
    if (id.isEmpty()) {
        return;
    }
    // 删除前按注册表里的**当前**引用数确认：本对话框改的是工作副本，通道的 source_id
    // 还留在 ConfigManager 里，所以"谁在用这条流"只能问 ConfigManager。
    const int users = ConfigManager::instance().videoSourceReferrerCount(id);
    if (users > 0) {
        const auto ans = QMessageBox::question(
            this, QStringLiteral("删除视频流"),
            QStringLiteral("这条流正被 %1 路通道引用。\n删除后这些通道将不再引用任何流"
                           "（内联地址还在的会继续用内联），确认删除？").arg(users),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (ans != QMessageBox::Yes) {
            return;
        }
    }
    for (int i = 0; i < streams_.size(); ++i) {
        if (streams_[i].id == id) {
            streams_.removeAt(i);
            break;
        }
    }
    if (!touched_.contains(id)) {
        touched_.append(id);
    }
    refreshList();   // 删掉的这条已经不在工作副本里，选中退到第一行
}

void StreamEditorDialog::browseUrl()
{
    const QString dir = urlEdit_->text().isEmpty()
            ? QDir::homePath()
            : QFileInfo(urlEdit_->text()).path();
    QFileDialog dialog(this, QStringLiteral("选择视频文件"), dir,
                       QStringLiteral("视频文件 (*.mp4 *.avi *.mkv *.mov *.flv *.ts *.webm);;所有文件 (*)"));
    dialog.setFileMode(QFileDialog::ExistingFile);
    if (dialog.exec() == QDialog::Accepted && !dialog.selectedFiles().isEmpty()) {
        urlEdit_->setText(dialog.selectedFiles().first());
        // 选了文件就把类型拨到 file：地址与类型不一致（file + rtsp://）是最常见的配错
        const int fileIdx = typeCombo_->findData(QStringLiteral("file"));
        if (fileIdx >= 0) {
            typeCombo_->setCurrentIndex(fileIdx);
        }
        applyToModel(currentId());
    }
}

void StreamEditorDialog::onAccept()
{
    applyToModel(currentId());

    // 地址为空的条目**当场拦下**，而不是"跳过、不写盘"：跳过之后配置里留的还是旧地址，
    // 界面上却是空的——用户看到的和存下来的不是一个东西。要么填上，要么选中它点「删除」。
    QStringList blanks;
    for (const VideoSourceCfg &s : streams_) {
        if (s.url.trimmed().isEmpty()) {
            blanks << s.id;
        }
    }
    if (!blanks.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("视频流地址为空"),
                             QStringLiteral("以下流还没有地址：%1\n请填上地址，或选中它点「删除」。")
                                 .arg(blanks.join(QStringLiteral("、"))));
        return;
    }

    ConfigManager &cfg = ConfigManager::instance();
    for (const VideoSourceCfg &s : streams_) {
        cfg.setVideoSource(s);
    }
    for (const QString &id : touched_) {
        bool stillExists = false;
        for (const VideoSourceCfg &s : streams_) {
            if (s.id == id) {
                stillExists = true;
                break;
            }
        }
        if (!stillExists) {
            cfg.removeVideoSource(id);   // 同时清掉引用它的通道的 source_id（不造悬空）
        }
    }
    // 上面全是改内存里的 root_，落盘只有这一句：setVideoSource/removeVideoSource
    // 都不自己写盘（与 ConfigManager 里所有 setter 同一口径），少了它关程序即丢。
    cfg.save();
    QDialog::accept();
}
