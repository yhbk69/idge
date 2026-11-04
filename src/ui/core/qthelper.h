#ifndef QTHELPER_H
#define QTHELPER_H

#include "head.h"

class QAbstractScrollArea;   // enableTouchScroll 的入参类型，这里只传指针，前置声明即可

/**
 * @file qthelper.h
 * @brief src/ui/core 的通用静态工具类声明（实现见 qthelper.cpp）。
 *
 * 本仓自己往这个移植进来的类里加过的只有三支，都在"触屏/文案"口径上：
 *   enableTouchScroll(All)（QScroller 挂 viewport）、dumpTouchGeometry
 *   （IDGE_TOUCHDUMP 命中区探针）、localizeMessageBoxButtons/localizeButtonBox/
 *   askConfirm/installMessageBoxTextFilter（标准按钮必须显式改中文，理由与实测
 *   数据见同目录 README「文案与按钮顺序」）。
 * 其余是 blacksoft 通用件：屏幕/DPI、居中、字体/编码/翻译、QSS 读取、
 * 文件对话框、格式化、无边框窗体等。取值档（字号/尺寸）一律在 theme.h，
 * 本类不定义任何设计令牌，别在这里加新常量。
 */

// ============================================================================
// QtHelper - Qt通用工具类
// 作用：封装常用的屏幕信息、窗口管理、文件操作、加密校验、样式设置等实用方法
// 线程：除纯计算那几个（getUuid/getOrCode/getCheckCode/getTimeString/
//   getSizeString/cutString/getRangeValue）外，全部只能在 GUI 线程调用。
// ============================================================================
class QtHelper
{
public:
    // ========================================================================
    // 屏幕信息相关
    // ========================================================================

    // 是否使用缩放系数
    static bool useRatio;

    // 获取所有屏幕区域
    // available: true返回可用区域(排除任务栏), false返回完整区域
    // full: true返回所有屏幕拼接后的虚拟区域, false返回各屏幕独立区域
    static QList<QRect> getScreenRects(bool available = true, bool full = false);

    // 获取当前鼠标所在屏幕的索引号
    static int getScreenIndex();

    // 获取当前鼠标所在屏幕的区域
    static QRect getScreenRect(bool available = true, bool full = false);

    // 获取屏幕缩放系数(DPI/96)
    // index: 屏幕索引,-1表示自动获取鼠标所在屏幕
    // devicePixel: true返回设备像素比, false返回逻辑DPI
    static qreal getScreenRatio(int index = -1, bool devicePixel = false);

    // ========================================================================
    // 窗口居中相关
    // ========================================================================

    // 矫正窗口区域使其居中显示在当前屏幕
    static QRect checkCenterRect(QRect &rect, bool available = true);

    // 获取桌面宽度
    static int deskWidth();

    // 获取桌面高度
    static int deskHeight();

    // 获取桌面尺寸
    static QSize deskSize();

    // 居中显示的参照窗体(为0则以桌面为参照)
    static QWidget *centerBaseForm;

    // 将窗体居中显示
    static void setFormInCenter(QWidget *form);

    // 显示窗体并居中,若超出屏幕则最大化
    static void showForm(QWidget *form);

    // ========================================================================
    // 程序路径信息
    // ========================================================================

    // 获取程序名称(不含路径和扩展名)
    static QString appName();

    // 获取程序所在目录路径
    static QString appPath();

    // 从argv获取程序路径和名称(支持中文路径)
    static void getCurrentInfo(char *argv[], QString &path, QString &name);

    // 从ini配置文件读取指定key的值
    static QString getIniValue(const QString &fileName, const QString &key);
    // 另一个重载：路径按 "程序所在目录 / dir + name + .ini" 拼，file 非空时覆盖 name。
    // ⚠ dir 是拼在文件名**前面的前缀**（中间没有 '/'），不是目录层级，别指望它能指到子目录。
    // 两个重载都**不报错**：文件不存在/没有这个 key 一律返回空串，调用方自己判空。
    // NOTE: 匹配用的是 startsWith(key)，所以 key="ip" 会连 "ip2=..." 一起命中——
    // 取前缀互相包含的多个 key 时要把名字写全。本仓配置全走 ConfigManager，
    // 上面这两个重载目前零调用。
    static QString getIniValue(char *argv[], const QString &key, const QString &dir = QString(), const QString &file = QString());

    // ========================================================================
    // 网络相关
    // ========================================================================

    // 获取本机所有IPv4网卡地址(过滤虚拟网卡)
    static QStringList getLocalIPs();

    // 初始化下拉框并填充网卡地址列表,设置默认IP
    static void initLocalIPs(QComboBox *cbox, const QString &defaultIP, bool local127 = true);

    // ========================================================================
    // 颜色相关
    // ========================================================================

    // 预定义颜色集合
    static QList<QColor> colors;

    // 获取预定义颜色列表
    static QList<QColor> getColorList();

    // 获取颜色名称列表(#RRGGBB格式)
    static QStringList getColorNames();

    // 随机获取一个预定义颜色
    static QColor getRandColor();

    // ========================================================================
    // 随机数相关
    // ========================================================================

    // 初始化随机数种子(基于当前时间)
    static void initRand();

    // 获取指定范围内的随机浮点数
    static float getRandFloat(float min, float max);

    // 获取指定范围内的随机整数
    // contansMin/contansMax: 是否包含最小/最大值
    static double getRandValue(int min, int max, bool contansMin = false, bool contansMax = false);

    // 在指定中心点周围生成随机经纬度点集合
    static QStringList getRandPoint(int count, float mainLng, float mainLat, float dotLng, float dotLat);

    // 根据旧范围值映射到新范围值
    static int getRangeValue(int oldMin, int oldMax, int oldValue, int newMin, int newMax);

    // ========================================================================
    // 通用工具
    // ========================================================================

    // 生成UUID字符串
    static QString getUuid();

    // 校验目录是否存在,不存在则创建(支持相对路径)
    static QString checkPath(const QString &dirName);

    // 将相对路径转换为完整路径
    static QString checkFile(const QString &fileName);

    // 通用延时函数(支持Qt4/Qt5/Qt6)
    // exec=true时主线程阻塞延时, exec=false时事件循环延时
    static void sleep(int msec, bool exec = true);

    // 检查程序是否已运行(通过共享内存实现,仅Windows)
    static void checkRun();

    // ========================================================================
    // 触屏拖动滚动（QScroller）
    // ========================================================================
    // 需求：手指在列表/日志/设置页上直接上下划，不再依赖 26px 的滚动条。
    //
    // 为什么是 LeftMouseButtonGesture：本机是 X11 + GNOME，电容屏的手指是以
    // **合成鼠标事件**进来的（没有 libinput touch 通道给 Qt），而 Qt 5.12 的
    // grabGesture 只接受**单个** gesture type（没有 6.x 的 MouseTouchGesture）。
    // 挂 TouchGesture 在这套环境下收不到事件，等于没挂。
    //
    // 代价（已接受并记录在 src/ui/core/README.md）：QScroller 一挂到 viewport 就
    // 接管该 viewport 的左键拖动，"按住拖选文字再复制"在日志区不再可用。
    static void enableTouchScroll(QAbstractScrollArea *area);

    // 扫描 root 自身 + 其全部子孙里的 QAbstractScrollArea，逐个挂上。
    // 必须在页面**全部建完之后**调（构造中途挂会漏掉后建的件）。
    static void enableTouchScrollAll(QWidget *root);

    // 触屏命中区审计：把 root 下所有**当前可见**的可点控件的真实高度打出来。
    // 只在 IDGE_TOUCHDUMP=1 时被调用（调用点在 frmMain 的轮询定时器里）。
    // 为什么不能靠算式：QSS 的 min-height 不含 padding/border，而 setFixedSize 会封顶、
    // 内联样式会打穿皮肤层——三条规则叠加后"看代码推算"经常和屏幕上的实际像素不符。
    static void dumpTouchGeometry(QWidget *root);

    // ========================================================================
    // 样式/字体/编码
    // ========================================================================

    // 设置Qt内置样式(Fusion/Cleanlooks)
    static void setStyle();

    // 加载自定义字体文件
    static QFont addFont(const QString &fontFile, const QString &fontName);

    // 设置全局字体(自动适配平台)
    static void setFont(int fontSize = 12);

    // 设置文本编码(默认UTF-8)
    static void setCode(bool utf8 = true);

    // 加载翻译文件(.qm)
    static void setTranslator(const QString &qmFile);

    /**
     * @brief 统一消息框标准按钮为中文
     *
     * 实测（本机 5.12.8，装了 src/ui/core/qrc/qm/qt_zh_CN.qm，用 qApp->translate 逐上下文验证）：
     *   - `QPlatformTheme` 这个上下文**一条都没译**（Cancel/&Yes/&No 原样返回英文），
     *     而 QMessageBox 与 QDialogButtonBox 的默认按钮文案都从这里取——所以两类框都是英文，
     *     "QDialogButtonBox 上下文被词典译全了"那条老判断不成立（它根本没被用到）。
     *   - QMessageBox 上下文只有 "OK"→"确定" 这一条对得上。
     * 结论：凡是标准按钮，**必须显式改写**，不能指望 .qm；细节见同目录 README「文案与按钮顺序」。
     */
    static void localizeMessageBoxButtons(QMessageBox *box);

    /**
     * @brief 统一 QDialogButtonBox 的角色默认文案为中文（判据同 localizeMessageBoxButtons）
     *
     * 只改"文案还是 Qt 那份英文"的按钮；调用方自己写过中文的（含 &助记符）一律不动，
     * 这样信息框里"确认误报 / 不是误报"这类专用文案不会被改成通用的"是 / 否"。
     * 顺序不用管：Qt 按 ButtonRole 排位，实测 `addButton(Cancel)` 再 `addButton(Ok)`
     * 出来的仍是「确定在左、取消在右」，正好是本项目的口径。
     */
    static void localizeButtonBox(QDialogButtonBox *box);

    // 询问框：固定"确定/取消"两个按钮，返回 QMessageBox::Ok 表示用户确认。
    // 新增询问一律走这里，不要直接 QMessageBox::question（那会给英文 Yes/No，
    // 靠全局过滤器兜底也只能译成"是/否"，与本项目的"确定/取消"文案不一致）。
    static QMessageBox::StandardButton askConfirm(QWidget *parent, const QString &title,
                                                  const QString &text);

    // 挂一个全局事件过滤器：任何 QMessageBox / QDialogButtonBox / QFileDialog 显示前
    // 自动走上面几个改写函数。静态版 QMessageBox::information/warning/critical 与
    // getOpenFileName 一堆调用点没法逐个改，靠过滤器兜底；
    // 已显式写成中文的按钮不会被覆盖。
    static void installMessageBoxTextFilter();

    // ========================================================================
    // Android权限相关
    // ========================================================================

    // 动态申请单个Android权限
    static bool checkPermission(const QString &permission);

    // 一次性申请所有常用Android权限
    static void initAndroidPermission();

    // ========================================================================
    // 初始化相关
    // ========================================================================

    // 一次性初始化所有设置(编码、样式、字体、翻译等)
    static void initAll(bool utf8 = true, bool style = true, bool tabCenter = true, int fontSize = 13);

    // 初始化main函数最早执行的代码(高分屏、OpenGL等)
    static void initMain(bool desktopSettingsAware = false, bool use96Dpi = false, bool logCritical = true);

    // 初始化OpenGL类型
    // type: 1=桌面OpenGL, 2=OpenGLES, 3=软件渲染
    static void initOpenGL(quint8 type = 0, bool checkCardEnable = false, bool checkVirtualSystem = false);

    // ========================================================================
    // QSS样式表
    // ========================================================================

    // 读取qss文件内容
    static QString getStyle(const QString &qssFile);

    // 将qss文件设置为全局样式
    static void setStyle(const QString &qssFile);

    // ========================================================================
    // 命令行/系统检测
    // ========================================================================

    // 执行外部命令并返回标准输出
    static QString doCmd(const QString &program, const QStringList &arguments, int timeout = 1000);

    // 检测显卡是否被禁用(通过wmic命令)
    static bool isVideoCardEnable();

    // 检测是否运行在虚拟机环境中
    static bool isVirtualSystem();

    // ========================================================================
    // 消息日志
    // ========================================================================

    // 是否替换消息中的回车换行符
    static bool replaceCRLF;

    // 消息类型编号集合
    static QVector<int> msgTypes;

    // 消息类型名称集合(发送/接收/解析/错误/提示)
    static QVector<QString> msgKeys;

    // 消息类型对应颜色集合
    static QVector<QColor> msgColors;

    // 向文本框追加带时间戳和颜色的消息
    // clear: 清空文本框; pause: 暂停追加
    static QString appendMsg(QTextEdit *textEdit, int type, const QString &data,
                             int maxCount, int &currentCount,
                             bool clear = false, bool pause = false);

    // ========================================================================
    // 无边框窗体
    // ========================================================================

    // 设置窗体为无边框模式
    // tool: 工具窗口; top: 置顶; menu: 显示系统菜单按钮
    static void setFramelessForm(QWidget *widgetMain, bool tool = false, bool top = false, bool menu = true);

    // ========================================================================
    // 消息对话框
    // ========================================================================

    // 通用消息框(0=信息, 1=错误, 2=询问)
    static int showMessageBox(const QString &text, int type = 0, int closeSec = 0, bool exec = false);

    // 显示信息提示框
    static void showMessageBoxInfo(const QString &text, int closeSec = 0, bool exec = false);

    // 显示错误提示框
    static void showMessageBoxError(const QString &text, int closeSec = 0, bool exec = false);

    // 显示询问框(是/否),返回按钮编号
    static int showMessageBoxQuestion(const QString &text);

    // ========================================================================
    // 文件对话框
    // ========================================================================

    // 初始化文件对话框(标题、标签、目录、尺寸等)
    static void initDialog(QFileDialog *dialog, const QString &title, const QString &acceptName,
                           const QString &dirName, bool native, int width, int height);

    // 获取对话框结果(自动补全扩展名)
    static QString getDialogResult(QFileDialog *dialog);

    // 打开文件对话框
    static QString getOpenFileName(const QString &filter = QString(),
                                   const QString &dirName = QString(),
                                   const QString &fileName = QString(),
                                   bool native = false, int width = 900, int height = 600);

    // 保存文件对话框
    static QString getSaveFileName(const QString &filter = QString(),
                                   const QString &dirName = QString(),
                                   const QString &fileName = QString(),
                                   bool native = false, int width = 900, int height = 600);

    // 选择目录对话框
    static QString getExistingDirectory(const QString &dirName = QString(),
                                        bool native = false, int width = 900, int height = 600);

    // ========================================================================
    // 加密/校验
    // ========================================================================

    // 异或加密/解密(单字符密钥,对称操作)
    // 中文需先转base64编码
    static QString getXorEncryptDecrypt(const QString &value, char key);

    // 异或校验(所有字节异或)
    static quint8 getOrCode(const QByteArray &data);

    // 校验码(所有字节求和取模256)
    static quint8 getCheckCode(const QByteArray &data);

    // ========================================================================
    // 表格初始化
    // ========================================================================

    // 初始化QTableView公共属性(行高、选中行为、编辑模式等)
    static void initTableView(QTableView *tableView, int rowHeight = 25,
                              bool headVisible = false, bool edit = false,
                              bool stretchLast = true);

    // 打开文件并弹出确认框
    static void openFile(const QString &fileName, const QString &msg);

    // 检查ini配置文件是否完整(每个key都有value)
    static bool checkIniFile(const QString &iniFile);

    // 首尾截断字符串显示(用于文件名过长时的省略显示)
    // left: 保留左侧字符数, right: 保留右侧字符数, file: 是否去掉扩展名
    static QString cutString(const QString &text, int len, int left, int right, bool file, const QString &mid = "...");

    // ========================================================================
    // 图片缩放/居中
    // ========================================================================

    // 根据图片尺寸和窗体区域计算居中缩放后的区域
    // scaleMode: 0=自动调整(仅放大), 1=等比缩放, 2=拉伸填充
    static QRect getCenterRect(const QSize &imageSize, const QRect &widgetRect, int borderWidth = 2, int scaleMode = 0);

    // 缩放图片到指定尺寸
    // fast: true=快速缩放, false=平滑缩放
    static void getScaledImage(QImage &image, const QSize &widgetSize, int scaleMode = 0, bool fast = true);

    // ========================================================================
    // 时间/大小格式化
    // ========================================================================

    // 毫秒数转MM:SS格式
    static QString getTimeString(qint64 time);

    // QElapsedTimer转秒数(保留3位小数)
    static QString getTimeString(QElapsedTimer timer);

    // 文件大小转可读字符串(KB/MB/GB/TB)
    static QString getSizeString(quint64 size);

    // ========================================================================
    // 系统设置
    // ========================================================================

    // 设置系统日期时间
    static void setSystemDateTime(const QString &year, const QString &month, const QString &day,
                                  const QString &hour, const QString &min, const QString &sec);

    // 设置开机自启动(写入注册表)
    static void runWithSystem(bool autoRun = true);
    // 另一个重载：登记的是指定程序（fileName 作注册表键名、filePath 作值）。
    // 上面那个不带文件名的版本只是拿 qApp 的 applicationName/applicationFilePath
    // 转调这里，两者行为完全一致。
    // ⚠ 只有 Q_OS_WIN 有实现体：在本板（Linux）上两个重载都是**空函数**——
    //   不报错、不写任何配置、也不返回成败。板上要配自启得改 systemd/桌面 autostart，
    //   别在这个类里加 Linux 分支（那是平台集成，不是本类的职责）。
    static void runWithSystem(const QString &fileName, const QString &filePath, bool autoRun = true);

    // 启动外部程序(已在运行则不重复启动)
    static void start(const QString &path, const QString &name, bool bin = true);
};

#endif // QTHELPER_H
