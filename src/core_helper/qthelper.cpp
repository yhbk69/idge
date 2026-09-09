#include "qthelper.h"
#include <QNetworkInterface>
#include <QNetworkProxy>
#include <QWidget>

// 时间戳宏,用于调试输出
#define TIMEMS qPrintable(QTime::currentTime().toString("HH:mm:ss zzz"))

bool QtHelper::useRatio = true;

// ============================================================================
// 屏幕信息相关
// ============================================================================

// 获取所有屏幕区域
// full: true返回所有屏幕拼接的虚拟区域, false返回各屏幕独立区域
QList<QRect> QtHelper::getScreenRects(bool available, bool full)
{
    QRect rect;
    QList<QRect> rects;
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
    // Qt5及以上使用QScreen获取屏幕信息
    QList<QScreen *> screens = qApp->screens();
    int screenCount = screens.count();
    for (int i = 0; i < screenCount; ++i) {
        QScreen *screen = screens.at(i);
        if (full) {
            // virtualGeometry返回所有屏幕拼接后的虚拟区域
            rect = (available ? screen->availableVirtualGeometry() : screen->virtualGeometry());
        } else {
            // geometry返回单个屏幕区域
            rect = (available ? screen->availableGeometry() : screen->geometry());
        }

        // 根据设备像素比缩放宽高(处理高分屏)
        qreal ratio = (QtHelper::useRatio ? screen->devicePixelRatio() : 1);
        rect.setWidth(rect.width() * ratio);
        rect.setHeight(rect.height() * ratio);
        rects << rect;
    }
#else
    // Qt4使用QDesktopWidget获取屏幕信息
    QDesktopWidget *desk = qApp->desktop();
    int screenCount = desk->screenCount();
    for (int i = 0; i < screenCount; ++i) {
        if (full) {
            rect = (available ? desk->geometry() : desk->geometry());
        } else {
            rect = (available ? desk->availableGeometry(i) : desk->screenGeometry(i));
        }

        rects << rect;
    }
#endif
    return rects;
}

// 获取当前鼠标所在屏幕的索引号
int QtHelper::getScreenIndex()
{
    int screenIndex = 0;
    // 获取所有屏幕区域(非可用区域,用于精确判断)
    QList<QRect> rects = getScreenRects(false);
    int count = rects.count();
    for (int i = 0; i < count; ++i) {
        // 判断鼠标光标当前位置是否在第i个屏幕区域内
        QPoint pos = QCursor::pos();
        if (rects.at(i).contains(pos)) {
            screenIndex = i;
            break;
        }
    }

    return screenIndex;
}

// 获取当前鼠标所在屏幕的区域
QRect QtHelper::getScreenRect(bool available, bool full)
{
    int screenIndex = getScreenIndex();
    QList<QRect> rects = getScreenRects(available, full);
    return rects.at(screenIndex);
}

// 获取屏幕缩放系数(DPI/96)
// index=-1表示自动获取鼠标所在屏幕
qreal QtHelper::getScreenRatio(int index, bool devicePixel)
{
    qreal ratio = 1.0;
    // 自动获取当前鼠标所在屏幕索引
    int screenIndex = (index == -1 ? getScreenIndex() : index);
#if (QT_VERSION >= QT_VERSION_CHECK(5,5,0))
    QScreen *screen = qApp->screens().at(screenIndex);
    if (devicePixel) {
        // 设备像素比(需开启AA_EnableHighDpiScaling)
        ratio = screen->devicePixelRatio() * 96;
    } else {
        // 逻辑DPI值
        ratio = screen->logicalDotsPerInch();
    }
#else
    // Qt4不支持动态识别缩放变化
    ratio = qApp->desktop()->screen(screenIndex)->logicalDpiX();
#endif
    // 除以96得到缩放倍数(1.0表示100%)
    return ratio / 96;
}

// 矫正窗口区域使其居中显示在当前屏幕
QRect QtHelper::checkCenterRect(QRect &rect, bool available)
{
    QRect deskRect = QtHelper::getScreenRect(available);
    int formWidth = rect.width();
    int formHeight = rect.height();
    int deskWidth = deskRect.width();
    int deskHeight = deskRect.height();
    // 计算居中位置(水平居中 + 垂直居中)
    int formX = deskWidth / 2 - formWidth / 2 + deskRect.x();
    int formY = deskHeight / 2 - formHeight / 2;
    rect = QRect(formX, formY, formWidth, formHeight);
    return deskRect;
}

// 获取桌面宽度
int QtHelper::deskWidth()
{
    return getScreenRect().width();
}

// 获取桌面高度
int QtHelper::deskHeight()
{
    return getScreenRect().height();
}

// 获取桌面尺寸
QSize QtHelper::deskSize()
{
    return getScreenRect().size();
}

// ============================================================================
// 窗口居中显示
// ============================================================================

// 居中显示的参照窗体(为0则以桌面为参照)
QWidget *QtHelper::centerBaseForm = 0;

// 将窗体居中显示
void QtHelper::setFormInCenter(QWidget *form)
{
    int formWidth = form->width();
    int formHeight = form->height();

    // centerBaseForm为空时以桌面屏幕为参照,否则以参照窗体为参照
    QRect rect;
    if (centerBaseForm == 0) {
        rect = getScreenRect();
    } else {
        rect = centerBaseForm->geometry();
    }

    int deskWidth = rect.width();
    int deskHeight = rect.height();
    // 计算居中坐标并移动窗体
    QPoint movePoint(deskWidth / 2 - formWidth / 2 + rect.x(), deskHeight / 2 - formHeight / 2 + rect.y());
    form->move(movePoint);
}

// 显示窗体并居中,若超出屏幕分辨率则最大化
void QtHelper::showForm(QWidget *form)
{
    setFormInCenter(form);
    form->show();

    // 窗体尺寸超出屏幕时自动最大化
    if (form->width() + 20 > deskWidth() || form->height() + 50 > deskHeight()) {
        QMetaObject::invokeMethod(form, "showMaximized", Qt::QueuedConnection);
    }
}

// ============================================================================
// 程序路径信息
// ============================================================================

// 获取程序名称(不含路径和扩展名)
// 使用static缓存,仅首次调用时获取
QString QtHelper::appName()
{
    static QString name;
    if (name.isEmpty()) {
        name = qApp->applicationFilePath();
        // 过滤安卓路径中的架构后缀(如_armeabi-v7a/_arm64-v8a)
        QStringList list = name.split("/");
        name = list.at(list.count() - 1).split(".").at(0);
        name.replace("_armeabi-v7a", "");
        name.replace("_arm64-v8a", "");
    }

    return name;
}

// 获取程序所在目录路径
// 安卓平台返回外部存储下的应用专属目录
QString QtHelper::appPath()
{
    static QString path;
    if (path.isEmpty()) {
#ifdef Q_OS_ANDROID
        // 安卓默认外部存储根目录
        path = "/storage/emulated/0";
        // 加上程序名作为子目录,前加0方便排序
        path = path + "/0" + appName();
#else
        path = qApp->applicationDirPath();
#endif
    }

    return path;
}

// 从argv获取程序路径和名称(支持中文路径)
void QtHelper::getCurrentInfo(char *argv[], QString &path, QString &name)
{
    // fromLocal8Bit确保中文路径正常处理
    QString argv0 = QString::fromLocal8Bit(argv[0]);
    QFileInfo file(argv0);
    path = file.path();
    name = file.baseName();
}

// 从ini配置文件逐行读取指定key的值
QString QtHelper::getIniValue(const QString &fileName, const QString &key)
{
    QString value;
    QFile file(fileName);
    if (file.open(QFile::ReadOnly | QFile::Text)) {
        while (!file.atEnd()) {
            QString line = file.readLine();
            // 查找以key开头的行
            if (line.startsWith(key)) {
                line = line.replace("\n", "");
                line = line.trimmed();
                // 按等号分割取最后一个值
                value = line.split("=").last();
                break;
            }
        }
    }
    return value;
}

// 从程序目录下的ini文件读取配置值
QString QtHelper::getIniValue(char *argv[], const QString &key, const QString &dir, const QString &file)
{
    QString path, name;
    QtHelper::getCurrentInfo(argv, path, name);
    // 支持指定文件名(防止程序重命名后找不到配置)
    if (!file.isEmpty()) {
        name = file;
    }

    QString fileName = QString("%1/%2%3.ini").arg(path).arg(dir).arg(name);
    return getIniValue(fileName, key);
}

// ============================================================================
// 网络相关
// ============================================================================

// 获取本机所有IPv4网卡地址(过滤虚拟网卡)
QStringList QtHelper::getLocalIPs()
{
    static QStringList ips;
    if (ips.count() == 0) {
#ifdef Q_OS_WASM
        // WebAssembly环境只返回本地回环地址
        ips << "127.0.0.1";
#else
        QList<QNetworkInterface> netInterfaces = QNetworkInterface::allInterfaces();
        foreach (QNetworkInterface netInterface, netInterfaces) {
            // 过滤VMware和Npcap等虚拟网卡
            QString humanReadableName = netInterface.humanReadableName().toLower();
            if (humanReadableName.startsWith("vmware network adapter") || humanReadableName.startsWith("npcap loopback adapter")) {
                continue;
            }

            // 只保留已启用且正在运行的网络接口
            bool flag = (netInterface.flags() == (QNetworkInterface::IsUp | QNetworkInterface::IsRunning | QNetworkInterface::CanBroadcast | QNetworkInterface::CanMulticast));
            if (!flag) {
                continue;
            }

            QList<QNetworkAddressEntry> addrs = netInterface.addressEntries();
            foreach (QNetworkAddressEntry addr, addrs) {
                // 只取IPv4地址
                if (addr.ip().protocol() != QAbstractSocket::IPv4Protocol) {
                    continue;
                }

                QString ip4 = addr.ip().toString();
                // 排除本地回环地址
                if (ip4 != "127.0.0.1") {
                    ips << ip4;
                }
            }
        }
#endif
    }

    return ips;
}

// 初始化下拉框并填充网卡地址列表
// local127: 是否包含127.0.0.1
void QtHelper::initLocalIPs(QComboBox *cbox, const QString &defaultIP, bool local127)
{
    QStringList ips;
    if (local127) {
        ips << "127.0.0.1";
    }

    // 追加所有本机网卡地址
    ips << QtHelper::getLocalIPs();

    // 默认IP不在列表中则取第一个
    QString ip = defaultIP;
    if (ips.count() > 0) {
        ip = ips.contains(ip) ? ip : ips.first();
    }

    // 设置下拉框当前项
    int index = ips.indexOf(ip);
    cbox->addItems(ips);
    cbox->setCurrentIndex(index < 0 ? 0 : index);

    // 如果有可编辑文本框,同步设置文本值
    if (cbox->lineEdit()) {
        cbox->lineEdit()->setText(ip);
    }
}

// ============================================================================
// 颜色相关
// ============================================================================

// 预定义颜色集合(延迟初始化)
QList<QColor> QtHelper::colors = QList<QColor>();

// 获取预定义颜色列表
QList<QColor> QtHelper::getColorList()
{
    if (colors.count() == 0) {
        // 15种预定义颜色,可自行扩展
        colors << QColor(0, 176, 180) << QColor(0, 113, 193) << QColor(255, 192, 0);
        colors << QColor(72, 103, 149) << QColor(185, 87, 86) << QColor(0, 177, 125);
        colors << QColor(214, 77, 84) << QColor(71, 164, 233) << QColor(34, 163, 169);
        colors << QColor(59, 123, 156) << QColor(162, 121, 197) << QColor(72, 202, 245);
        colors << QColor(0, 150, 121) << QColor(111, 9, 176) << QColor(250, 170, 20);
    }

    return colors;
}

// 获取颜色名称列表(#RRGGBB格式)
QStringList QtHelper::getColorNames()
{
    QList<QColor> colors = getColorList();
    QStringList colorNames;
    foreach (QColor color, colors) {
        colorNames << color.name();
    }
    return colorNames;
}

// 随机获取一个预定义颜色
QColor QtHelper::getRandColor()
{
    QList<QColor> colors = getColorList();
    // getRandValue生成包含两端的随机索引
    int index = getRandValue(0, colors.count(), true);
    return colors.at(index);
}

// ============================================================================
// 随机数相关
// ============================================================================

// 初始化随机数种子(基于当前时间的毫秒和秒)
void QtHelper::initRand()
{
    QTime t = QTime::currentTime();
    srand(t.msec() + t.second() * 1000);
}

// 获取指定范围内的随机浮点数
float QtHelper::getRandFloat(float min, float max)
{
    double diff = fabs(max - min);
    // 生成0.0~1.0之间的随机比例
    double value = (double)(rand() % 100) / 100;
    value = min + value * diff;
    return value;
}

// 获取指定范围内的随机整数
// contansMin/contansMax控制是否包含边界值
double QtHelper::getRandValue(int min, int max, bool contansMin, bool contansMax)
{
    int value;
#if (QT_VERSION <= QT_VERSION_CHECK(5,10,0))
    // Qt5.10及以下使用rand()
    // 通用公式: a + rand() % n (a为起始值,n为范围)
    if (contansMin) {
        if (contansMax) {
            // [min, max] 包含两端
            value = min + 0 + (rand() % (max - min + 1));
        } else {
            // [min, max) 包含最小值
            value = min + 0 + (rand() % (max - min + 0));
        }
    } else {
        if (contansMax) {
            // (min, max] 包含最大值
            value = min + 1 + (rand() % (max - min + 0));
        } else {
            // (min, max) 不包含两端
            value = min + 1 + (rand() % (max - min - 1));
        }
    }
#else
    // Qt5.10+使用QRandomGenerator
    if (contansMin) {
        if (contansMax) {
            value = QRandomGenerator::global()->bounded(min + 0, max + 1);
        } else {
            value = QRandomGenerator::global()->bounded(min + 0, max + 0);
        }
    } else {
        if (contansMax) {
            value = QRandomGenerator::global()->bounded(min + 1, max + 1);
        } else {
            value = QRandomGenerator::global()->bounded(min + 1, max + 0);
        }
    }
#endif
    return value;
}

// 在指定中心点周围生成随机经纬度点集合
// dotLng/dotLat: 经度/纬度方向的最大偏移量
QStringList QtHelper::getRandPoint(int count, float mainLng, float mainLat, float dotLng, float dotLat)
{
    QStringList points;
    for (int i = 0; i < count; ++i) {
#if (QT_VERSION >= QT_VERSION_CHECK(5,10,0))
        // Qt5.10+使用QRandomGenerator
        float lngx = QRandomGenerator::global()->bounded(dotLng);
        float latx = QRandomGenerator::global()->bounded(dotLat);
#else
        // Qt4/5.9使用自定义随机函数,偏移量在1%~100%之间
        float lngx = getRandFloat(dotLng / 100, dotLng);
        float latx = getRandFloat(dotLat / 100, dotLat);
#endif
        // 精度转为8位小数的字符串
        QString lng2 = QString::number(mainLng + lngx, 'f', 8);
        QString lat2 = QString::number(mainLat + latx, 'f', 8);
        QString point = QString("%1,%2").arg(lng2).arg(lat2);
        points << point;
    }

    return points;
}

// 根据旧范围值映射到新范围值(线性映射)
int QtHelper::getRangeValue(int oldMin, int oldMax, int oldValue, int newMin, int newMax)
{
    return (((oldValue - oldMin) * (newMax - newMin)) / (oldMax - oldMin)) + newMin;
}

// ============================================================================
// 通用工具
// ============================================================================

// 生成UUID字符串(去掉花括号)
QString QtHelper::getUuid()
{
    QString uuid = QUuid::createUuid().toString();
    uuid.replace("{", "");
    uuid.replace("}", "");
    return uuid;
}

// 校验目录路径,不存在则创建
// 支持相对路径(./开头或纯相对路径)
QString QtHelper::checkPath(const QString &dirName)
{
    QString path = dirName;
    // ./开头的相对路径转换为基于appPath的完整路径
    if (path.startsWith("./")) {
        path.replace(".", "");
        path = QtHelper::appPath() + path;
    } else if (!path.startsWith("/") && !path.contains(":/")) {
        // 纯相对路径也补全为完整路径
        path = QtHelper::appPath() + "/" + path;
    }

    // 目录不存在时递归创建
    QDir dir(path);
    if (!dir.exists()) {
        dir.mkpath(path);
    }

    return path;
}

// 将相对路径转换为完整路径
QString QtHelper::checkFile(const QString &fileName)
{
    QString name = fileName;
    if (name.startsWith("./")) {
        name = QtHelper::appPath() + name.mid(1, name.length());
    }

    return name;
}

// 通用延时函数
// exec=true: 阻塞式延时(主线程会卡住)
// exec=false: 事件循环式延时(不卡界面)
void QtHelper::sleep(int msec, bool exec)
{
    if (msec <= 0) {
        return;
    }

    if (exec) {
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
        // Qt5+使用QThread::msleep阻塞延时
        QThread::msleep(msec);
#else
        // Qt4使用事件循环轮询方式延时
        QTime endTime = QTime::currentTime().addMSecs(msec);
        while (QTime::currentTime() < endTime) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        }
#endif
    } else {
        // 使用QEventLoop实现非阻塞延时
        QEventLoop loop;
        QTimer::singleShot(msec, &loop, SLOT(quit()));
        loop.exec();
    }
}

// 检查程序是否已运行(通过共享内存实现,仅Windows)
void QtHelper::checkRun()
{
#ifdef Q_OS_WIN
    // 延时1秒等待前一个实例释放共享内存
    QtHelper::sleep(1000);
    // 以程序名为共享内存key
    static QSharedMemory mem(QtHelper::appName());
    // create失败说明共享内存已存在,程序已在运行
    if (!mem.create(1)) {
        QtHelper::showMessageBoxError("程序已运行, 软件将自动关闭!", 5, true);
        exit(0);
    }
#endif
}

// ============================================================================
// 样式/字体/编码
// ============================================================================

// 设置Qt内置样式
void QtHelper::setStyle()
{
    // Qt5+使用Fusion风格,Qt4使用Cleanlooks风格
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
    qApp->setStyle("Fusion");
#else
    qApp->setStyle("Cleanlooks");
#endif

    // 设置窗口背景色
    QPalette palette;
    palette.setBrush(QPalette::Window, QColor("#F0F0F0"));
    //qApp->setPalette(palette);
}

// 加载自定义字体文件
// 如果字体已存在则不重复加载
QFont QtHelper::addFont(const QString &fontFile, const QString &fontName)
{
    QFontDatabase fontDb;
    // 检查字体是否已注册
    if (!fontDb.families().contains(fontName)) {
        int fontId = fontDb.addApplicationFont(fontFile);
        QStringList listName = fontDb.applicationFontFamilies(fontId);
        if (listName.count() == 0) {
            qDebug() << QString("load %1 error").arg(fontName);
        }
    }

    // 创建字体对象并禁用字体提示(提高渲染质量)
    QFont font;
    if (fontDb.families().contains(fontName)) {
        font = QFont(fontName);
#if (QT_VERSION >= QT_VERSION_CHECK(4,8,0))
        font.setHintingPreference(QFont::PreferNoHinting);
#endif
    }

    return font;
}

// 设置全局字体(自动适配各平台)
void QtHelper::setFont(int fontSize)
{
    // Android和WebAssembly需要手动加载中文字体
#if (defined Q_OS_ANDROID) || (defined Q_OS_WASM)
    QString fontFile = ":/font/DroidSansFallback.ttf";
    QString fontName = "Droid Sans Fallback";
    qApp->setFont(addFont(fontFile, fontName));
    return;
#endif

    // ARM设备使用较大字体
#ifdef __arm__
    fontSize = 25;
#endif

    QStringList preferredFonts;
#ifdef Q_OS_WIN
    // Windows优先字体列表
    preferredFonts << "Microsoft YaHei" << "SimHei" << "SimSun";
#elif defined(Q_OS_MAC)
    // macOS优先字体列表
    preferredFonts << "PingFang SC" << "Heiti SC" << "STHeiti";
#else
    // Linux: 先尝试从本地文件加载字体
    QStringList localFontFiles;
    localFontFiles << "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc";
    localFontFiles << "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf";

    QString fontFile;
    for (const QString &f : localFontFiles) {
        if (QFile::exists(f)) {
            fontFile = f;
            break;
        }
    }

    if (!fontFile.isEmpty()) {
        QFontDatabase fontDb;
        int fontId = fontDb.addApplicationFont(fontFile);
        QStringList families = fontDb.applicationFontFamilies(fontId);
        if (!families.isEmpty()) {
            QFont font(families.first());
            font.setPixelSize(fontSize);
#if (QT_VERSION >= QT_VERSION_CHECK(4,8,0))
            font.setHintingPreference(QFont::PreferNoHinting);
#endif
            qApp->setFont(font);
            return;
        }
    }

    // Linux备选字体列表
    preferredFonts << "Noto Sans CJK SC" << "Noto Sans SC"
                   << "Source Han Sans SC" << "WenQuanYi Micro Hei"
                   << "AR PL UMing CN" << "AR PL UKai CN"
                   << "Droid Sans Fallback";
#endif

    // 从系统已安装字体中查找第一个匹配的
    QString family;
    QFontDatabase db;
    for (const QString &f : preferredFonts) {
        if (db.families().contains(f)) {
            family = f;
            break;
        }
    }
    // 没有匹配字体则使用系统默认字体
    if (family.isEmpty()) {
        family = qApp->font().family();
    }

    QFont font;
    font.setFamily(family);
    font.setPixelSize(fontSize);
    qApp->setFont(font);
}

// 设置文本编码(默认UTF-8)
void QtHelper::setCode(bool utf8)
{
    QTextCodec *codec = QTextCodec::codecForName("utf-8");
#if (QT_VERSION < QT_VERSION_CHECK(5,0,0))
    // Qt4需要设置C字符串和Tr函数的编码
    QTextCodec::setCodecForCStrings(codec);
    QTextCodec::setCodecForTr(codec);
#endif

    // setCodecForLocale会影响toLocal8Bit函数的返回值
    if (utf8) {
        QTextCodec::setCodecForLocale(codec);
    }
}

// 加载翻译文件(.qm)
void QtHelper::setTranslator(const QString &qmFile)
{
    // 文件不存在则跳过
    if (!QFile(qmFile).exists()) {
        return;
    }

    QTranslator *translator = new QTranslator(qApp);
    if (translator->load(qmFile)) {
        qApp->installTranslator(translator);
    }
}

// ============================================================================
// Android权限相关
// ============================================================================

// Android头文件包含(Qt5和Qt6路径不同)
#ifdef Q_OS_ANDROID
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
#include <QtAndroidExtras>
#else
// Qt6将相关类移到了core模块
#include <QtCore/private/qandroidextras_p.h>
#endif
#endif

// 动态申请单个Android权限
bool QtHelper::checkPermission(const QString &permission)
{
#ifdef Q_OS_ANDROID
#if (QT_VERSION >= QT_VERSION_CHECK(5,10,0) && QT_VERSION < QT_VERSION_CHECK(6,0,0))
    // Qt5.10~Qt6.0使用QtAndroid
    QtAndroid::PermissionResult result = QtAndroid::checkPermission(permission);
    if (result == QtAndroid::PermissionResult::Denied) {
        // 权限被拒绝则同步请求
        QtAndroid::requestPermissionsSync(QStringList() << permission);
        result = QtAndroid::checkPermission(permission);
        if (result == QtAndroid::PermissionResult::Denied) {
            return false;
        }
    }
#else
    // Qt6使用QtAndroidPrivate
    QFuture<QtAndroidPrivate::PermissionResult> result = QtAndroidPrivate::requestPermission(permission);
    if (result.resultAt(0) == QtAndroidPrivate::PermissionResult::Denied) {
        return false;
    }
#endif
#endif
    return true;
}

// 一次性申请所有常用Android权限
void QtHelper::initAndroidPermission()
{
    checkPermission("android.permission.CALL_PHONE");
    checkPermission("android.permission.SEND_SMS");
    checkPermission("android.permission.CAMERA");
    checkPermission("android.permission.READ_EXTERNAL_STORAGE");
    checkPermission("android.permission.WRITE_EXTERNAL_STORAGE");

    checkPermission("android.permission.ACCESS_COARSE_LOCATION");
    checkPermission("android.permission.INTERNET");
    checkPermission("android.permission.BLUETOOTH");
    checkPermission("android.permission.BLUETOOTH_SCAN");
    checkPermission("android.permission.BLUETOOTH_CONNECT");
    checkPermission("android.permission.BLUETOOTH_ADVERTISE");
}

// ============================================================================
// 初始化相关
// ============================================================================

// 一次性初始化所有设置
void QtHelper::initAll(bool utf8, bool style, bool tabCenter, int fontSize)
{
    QtHelper::initAndroidPermission();    // 初始化Android权限
    QtHelper::initRand();                 // 初始化随机数种子
    QtHelper::setCode(utf8);              // 设置编码
    QtHelper::setFont(fontSize);          // 设置字体

    if (style) {
        QtHelper::setStyle();             // 设置样式风格
    }

    // 选项卡居中显示
    if (tabCenter) {
        qApp->setStyleSheet("QTabWidget::tab-bar{alignment:center;}");
    }

    // 加载翻译文件
    QtHelper::setTranslator(":/qm/widgets.qm");
    QtHelper::setTranslator(":/qm/qt_zh_CN.qm");
    QtHelper::setTranslator(":/qm/designer_zh_CN.qm");

    // 不使用系统代理配置
    QNetworkProxyFactory::setUseSystemConfiguration(false);
    // 设置当前工作目录为程序所在目录
    QDir::setCurrent(QtHelper::appPath());
    // Qt4需要主动设置程序名称
#if (QT_VERSION < QT_VERSION_CHECK(5,0,0))
    qApp->setApplicationName(QtHelper::appName());
#endif
}

// Qt6 WebEngine头文件
#if (QT_VERSION >= QT_VERSION_CHECK(6,0,0))
#ifdef webengine
#include "qquickwindow.h"
#endif
#endif

// 初始化main函数最早执行的代码
// desktopSettingsAware: 是否应用操作系统字体等设置
// use96Dpi: 是否使用96DPI(禁用高分屏缩放)
// logCritical: 是否打印Qt内部警告信息
void QtHelper::initMain(bool desktopSettingsAware, bool use96Dpi, bool logCritical)
{
#ifdef Q_OS_LINUX
#ifndef Q_OS_ANDROID
    // Qt6默认使用wayland,无边框窗体需要xcb平台
    //qputenv("QT_QPA_PLATFORM", "xcb");
#endif
#endif

#ifdef webengine
    // 禁用WebEngine沙箱和安全策略(允许跨域请求)
    qputenv("QTWEBENGINE_DISABLE_SANDBOX", "1");
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", "--disable-web-security");
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
    // 是否应用操作系统设置(如系统字体)
    QApplication::setDesktopSettingsAware(desktopSettingsAware);
#endif

    // 安卓必须启用高分屏
#ifdef Q_OS_ANDROID
    use96Dpi = false;
#endif

    QtHelper::useRatio = use96Dpi;
#if (QT_VERSION >= QT_VERSION_CHECK(5,6,0) && QT_VERSION < QT_VERSION_CHECK(6,0,0))
    // Qt5.6~Qt6.0启用高分屏缩放支持
    if (!use96Dpi) {
        QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
        QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
    }
#endif

#ifdef Q_OS_WIN
    if (use96Dpi) {
        // 强制指定DPI为96(禁用缩放)
        qputenv("QT_FONT_DPI", "96");
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
        QApplication::setAttribute(Qt::AA_Use96Dpi);
#endif
    }
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(5,14,0))
    // 高分屏缩放策略: PassThrough表示不进行整数舍入
    QApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::PassThrough);
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
    // 禁用Qt内部警告信息输出
    if (!logCritical) {
        QLoggingCategory::setFilterRules("*.critical=false\n*.warning=false");
    }
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(5,4,0))
    // 设置OpenGL共享上下文(多窗口共享GL资源)
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#endif

#if (QT_VERSION >= QT_VERSION_CHECK(6,0,0))
#ifdef webengine
    // 修复Qt6中OpenGLWidget与WebEngine共存时的黑屏问题
    QQuickWindow::setGraphicsApi(QSGRendererInterface::OpenGL);
#endif
#endif
}

// 初始化OpenGL类型
// type: 1=桌面OpenGL, 2=OpenGLES, 3=软件渲染
void QtHelper::initOpenGL(quint8 type, bool checkCardEnable, bool checkVirtualSystem)
{
#if (QT_VERSION >= QT_VERSION_CHECK(5,4,0))
    // 根据type设置对应的OpenGL后端
    if (type == 1) {
        QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
    } else if (type == 2) {
        // OpenGLES禁用硬件加速(如ffmpeg的dxva2)
        QApplication::setAttribute(Qt::AA_UseOpenGLES);
    } else if (type == 3) {
        QApplication::setAttribute(Qt::AA_UseSoftwareOpenGL);
    }

    // 显卡被禁用时降级为OpenGLES
    if (checkCardEnable && !isVideoCardEnable()) {
        QApplication::setAttribute(Qt::AA_UseOpenGLES);
    }

    // 虚拟机环境降级为OpenGLES
    if (checkVirtualSystem && isVirtualSystem()) {
        QApplication::setAttribute(Qt::AA_UseOpenGLES);
    }
#endif
}

// ============================================================================
// QSS样式表
// ============================================================================

// 读取qss文件内容
QString QtHelper::getStyle(const QString &qssFile)
{
    QString qss;
    QFile file(qssFile);
    if (file.open(QFile::ReadOnly)) {
#if 0
        qss = QLatin1String(file.readAll());
#else
        // 逐行读取,不依赖文件编码
        QStringList list;
        QTextStream stream(&file);
        while (!stream.atEnd()) {
            QString line;
            stream >> line;
            list << line;
        }
        qss = list.join("\n");
#endif
    }

    return qss.trimmed();
}

// 将qss文件设置为全局样式
void QtHelper::setStyle(const QString &qssFile)
{
    QString qss = QtHelper::getStyle(qssFile);
    if (!qss.isEmpty()) {
        // 从qss第20个字符处提取调色板颜色(硬编码约定)
        QString paletteColor = qss.mid(20, 7);
        qApp->setPalette(QPalette(QColor(paletteColor)));
        qApp->setStyleSheet(qss);
    }
}

// ============================================================================
// 命令行/系统检测
// ============================================================================

// 执行外部命令并返回标准输出(去除换行符)
QString QtHelper::doCmd(const QString &program, const QStringList &arguments, int timeout)
{
    QString result;
#ifndef Q_OS_WASM
    QProcess p;
    p.start(program, arguments);
    p.waitForFinished(timeout);
    result = QString::fromLocal8Bit(p.readAllStandardOutput());
    // 清理换行符和多余空格
    result.replace("\r", "");
    result.replace("\n", "");
    result = result.simplified();
    result = result.trimmed();
#endif
    return result;
}

// 检测显卡是否被禁用(通过wmic命令查询)
bool QtHelper::isVideoCardEnable()
{
    QString result;
    bool videoCardEnable = true;

#if defined(Q_OS_WIN)
    QStringList args;
    // 查询显卡名称和状态
    args << "path" << "win32_VideoController" << "get" << "name,Status";
    result = doCmd("wmic", args);
#endif

    // 状态包含Error说明显卡被禁用
    if (result.contains("Error")) {
        videoCardEnable = false;
    }

    return videoCardEnable;
}

// 检测是否运行在虚拟机环境中
bool QtHelper::isVirtualSystem()
{
    QString result;
    bool virtualSystem = false;

#if defined(Q_OS_WIN)
    QStringList args;
    // 查询计算机型号
    args << "computersystem" << "get" << "Model";
    result = doCmd("wmic", args);
#elif defined(Q_OS_LINUX)
    QStringList args;
    // Linux使用lscpu命令
    result = doCmd("lscpu", args);
#endif

    // 检查是否包含虚拟机标识
    if (result.contains("VMware") || result.contains("VirtualBox") || result.contains("Alibaba")) {
        virtualSystem = true;
    }

    return virtualSystem;
}

// ============================================================================
// 消息日志
// ============================================================================

// 是否替换消息中的回车换行符
bool QtHelper::replaceCRLF = true;

// 消息类型编号: 0=发送, 1=接收, 2=解析, 3=错误, 4=提示
QVector<int> QtHelper::msgTypes = QVector<int>() << 0 << 1 << 2 << 3 << 4;

// 消息类型名称(中文)
QVector<QString> QtHelper::msgKeys = QVector<QString>() << QString::fromUtf8("发送") << QString::fromUtf8("接收") << QString::fromUtf8("解析") << QString::fromUtf8("错误") << QString::fromUtf8("提示");

// 消息类型对应颜色(绿色/红色/紫色/橙色/青色)
QVector<QColor> QtHelper::msgColors = QVector<QColor>() << QColor("#3BA372") << QColor("#EE6668") << QColor("#9861B4") << QColor("#FA8359") << QColor("#22A3A9");

// 向文本框追加带时间戳和颜色的消息
// clear: 清空文本框并重置计数
// pause: 暂停追加(返回空字符串)
QString QtHelper::appendMsg(QTextEdit *textEdit, int type, const QString &data, int maxCount, int &currentCount, bool clear, bool pause)
{
    if (clear) {
        textEdit->clear();
        currentCount = 0;
        return QString();
    }

    if (pause) {
        return QString();
    }

    // 超过最大条数时清空重置
    if (currentCount >= maxCount) {
        textEdit->clear();
        currentCount = 0;
    }

    // 根据消息类型设置不同颜色
    QString strType;
    int index = msgTypes.indexOf(type);
    if (index >= 0) {
        strType = msgKeys.at(index);
        textEdit->setTextColor(msgColors.at(index));
    }

    // 可选:过滤回车换行符
    QString strData = data;
    if (replaceCRLF) {
        strData.replace("\r", "");
        strData.replace("\n", "");
    }

    // 拼接时间戳和消息内容
    strData = QString("时间[%1] %2: %3").arg(TIMEMS).arg(strType).arg(strData);
    textEdit->append(strData);
    currentCount++;
    return strData;
}

// ============================================================================
// 无边框窗体
// ============================================================================

// 设置窗体为无边框模式
void QtHelper::setFramelessForm(QWidget *widgetMain, bool tool, bool top, bool menu)
{
    // 设置自定义属性(用于qss样式选择器)
    widgetMain->setProperty("form", true);
    widgetMain->setProperty("canMove", true);

#ifdef __arm__
    // ARM平台需要额外绕过窗口管理器
    widgetMain->setWindowFlags(Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint);
#else
    widgetMain->setWindowFlags(Qt::FramelessWindowHint);
#endif
    if (tool) {
        // 工具窗口(不在任务栏显示)
        widgetMain->setWindowFlags(widgetMain->windowFlags() | Qt::Tool);
    }
    if (top) {
        // 置顶显示
        widgetMain->setWindowFlags(widgetMain->windowFlags() | Qt::WindowStaysOnTopHint);
    }
    if (menu) {
        // 显示系统菜单和最小化/最大化按钮(仅Windows)
#ifdef Q_OS_WIN
        widgetMain->setWindowFlags(widgetMain->windowFlags() | Qt::WindowSystemMenuHint | Qt::WindowMinMaxButtonsHint);
#endif
    }
}

// ============================================================================
// 消息对话框
// ============================================================================

// 通用消息框(type: 0=信息, 1=错误, 2=询问)
int QtHelper::showMessageBox(const QString &text, int type, int closeSec, bool exec)
{
    int result = 0;
    if (type == 0) {
        showMessageBoxInfo(text, closeSec, exec);
    } else if (type == 1) {
        showMessageBoxError(text, closeSec, exec);
    } else if (type == 2) {
        result = showMessageBoxQuestion(text);
    }

    return result;
}

// 显示信息提示框
void QtHelper::showMessageBoxInfo(const QString &text, int closeSec, bool exec)
{
    QMessageBox box(QMessageBox::Information, "提示", text);
    box.setStandardButtons(QMessageBox::Yes);
    box.button(QMessageBox::Yes)->setText("确 定");
    box.exec();
}

// 显示错误提示框
void QtHelper::showMessageBoxError(const QString &text, int closeSec, bool exec)
{
    QMessageBox box(QMessageBox::Critical, "错误", text);
    box.setStandardButtons(QMessageBox::Yes);
    box.button(QMessageBox::Yes)->setText("确 定");
    box.exec();
}

// 显示询问框(是/否),返回按钮编号
int QtHelper::showMessageBoxQuestion(const QString &text)
{
    QMessageBox box(QMessageBox::Question, "询问", text);
    box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
    box.button(QMessageBox::Yes)->setText("确 定");
    box.button(QMessageBox::No)->setText("取 消");
    return box.exec();
}

// ============================================================================
// 文件对话框
// ============================================================================

// 初始化文件对话框(汉化标签、设置目录、调整尺寸)
void QtHelper::initDialog(QFileDialog *dialog, const QString &title, const QString &acceptName,
                          const QString &dirName, bool native, int width, int height)
{
    dialog->setWindowTitle(title);
    // 汉化对话框标签文本
    dialog->setLabelText(QFileDialog::Accept, acceptName);
    dialog->setLabelText(QFileDialog::Reject, "取消(&C)");
    dialog->setLabelText(QFileDialog::LookIn, "查看");
    dialog->setLabelText(QFileDialog::FileName, "名称");
    dialog->setLabelText(QFileDialog::FileType, "类型");

    // 设置默认显示目录
    if (!dirName.isEmpty()) {
        dialog->setDirectory(dirName);
    }

    // 设置对话框尺寸
    if (width > 0 && height > 0) {
#ifdef Q_OS_ANDROID
        // 安卓根据横竖屏自适应尺寸
        bool horizontal = (QtHelper::deskWidth() > QtHelper::deskHeight());
        if (horizontal) {
            width = QtHelper::deskWidth() / 2;
            height = QtHelper::deskHeight() - 50;
        } else {
            width = QtHelper::deskWidth() - 10;
            height = QtHelper::deskHeight() / 2;
        }
#endif
        dialog->setFixedSize(width, height);
    }

    // 是否使用本地系统对话框
    dialog->setOption(QFileDialog::DontUseNativeDialog, !native);
}

// 获取对话框结果,自动补全扩展名
QString QtHelper::getDialogResult(QFileDialog *dialog)
{
    QString result;
    if (dialog->exec() == QFileDialog::Accepted) {
        result = dialog->selectedFiles().first();
        if (!result.contains(".")) {
            // 从文件类型过滤器中提取扩展名
            QString filter = dialog->selectedNameFilter();
            if (filter.contains("*.")) {
                filter = filter.split("(").last();
                filter = filter.mid(0, filter.length() - 1);
                // 取第一个扩展名(排除*.*)
                if (!filter.contains("*.*")) {
                    filter = filter.split(" ").first();
                    result = result + filter.mid(1, filter.length());
                }
            }
        }
    }
    return result;
}

// 打开文件对话框
QString QtHelper::getOpenFileName(const QString &filter, const QString &dirName, const QString &fileName,
                                  bool native, int width, int height)
{
    QFileDialog dialog;
    initDialog(&dialog, "打开文件", "选择(&S)", dirName, native, width, height);

    if (!filter.isEmpty()) {
        dialog.setNameFilter(filter);
    }

    // 设置默认选中的文件名
    dialog.selectFile(fileName);
    return getDialogResult(&dialog);
}

// 保存文件对话框
QString QtHelper::getSaveFileName(const QString &filter, const QString &dirName, const QString &fileName,
                                  bool native, int width, int height)
{
    QFileDialog dialog;
    initDialog(&dialog, "保存文件", "保存(&S)", dirName, native, width, height);

    if (!filter.isEmpty()) {
        dialog.setNameFilter(filter);
    }

    dialog.selectFile(fileName);
    // 设置模态类型(允许输入文件名)
    dialog.setWindowModality(Qt::WindowModal);
    // 置顶显示
    dialog.setWindowFlags(dialog.windowFlags() | Qt::WindowStaysOnTopHint);
    return getDialogResult(&dialog);
}

// 选择目录对话框
QString QtHelper::getExistingDirectory(const QString &dirName, bool native, int width, int height)
{
    QFileDialog dialog;
    initDialog(&dialog, "选择目录", "选择(&S)", dirName, native, width, height);
    dialog.setOption(QFileDialog::ReadOnly);
    // 仅显示目录
#if (QT_VERSION < QT_VERSION_CHECK(6,0,0))
    dialog.setFileMode(QFileDialog::DirectoryOnly);
#else
    dialog.setFileMode(QFileDialog::Directory);
#endif
    dialog.setOption(QFileDialog::ShowDirsOnly);
    return getDialogResult(&dialog);
}

// ============================================================================
// 加密/校验
// ============================================================================

// 异或加密/解密(单字符密钥,对称操作)
// 加密和解密使用相同的操作
QString QtHelper::getXorEncryptDecrypt(const QString &value, char key)
{
    // 密钥范围校正
    if (key < 0 || key >= 127) {
        key = 127;
    }

    // Qt5.9+输出的加密字符串前会加@String前缀,需要去掉
    QString result = value;
    if (result.startsWith("@String")) {
        result = result.mid(8, result.length() - 9);
    }

    // 逐字符与密钥异或
    for (int i = 0; i < result.length(); ++i) {
        result[i] = QChar(result.at(i).toLatin1() ^ key);
    }
    return result;
}

// 异或校验(所有字节异或得到一个字节)
quint8 QtHelper::getOrCode(const QByteArray &data)
{
    int len = data.length();
    quint8 result = 0;
    for (int i = 0; i < len; ++i) {
        result ^= data.at(i);
    }

    return result;
}

// 校验码(所有字节求和,取模256)
quint8 QtHelper::getCheckCode(const QByteArray &data)
{
    int len = data.length();
    quint8 temp = 0;
    for (int i = 0; i < len; ++i) {
        temp += data.at(i);
    }

    return temp % 256;
}

// ============================================================================
// 表格初始化
// ============================================================================

// 初始化QTableView公共属性
void QtHelper::initTableView(QTableView *tableView, int rowHeight, bool headVisible, bool edit, bool stretchLast)
{
    // 设置自定义属性(用于qss样式选择器)
    tableView->setProperty("model", true);
    tableView->setWordWrap(false);                         // 禁用自动换行
    tableView->setTextElideMode(Qt::ElideNone);            // 超长文本不显示省略号
    tableView->setAlternatingRowColors(false);              // 不交替行颜色
    tableView->verticalHeader()->setVisible(headVisible);  // 垂直表头可见性
    tableView->horizontalHeader()->setHighlightSections(false); // 表头选中不高亮
    tableView->horizontalHeader()->setStretchLastSection(stretchLast); // 最后一列拉伸
    tableView->horizontalHeader()->setMinimumSectionSize(0);  // 最小列宽
    tableView->horizontalHeader()->setFixedHeight(rowHeight); // 表头高度
    tableView->verticalHeader()->setDefaultSectionSize(rowHeight); // 默认行高
    tableView->setSelectionBehavior(QAbstractItemView::SelectRows);    // 选中整行
    tableView->setSelectionMode(QAbstractItemView::SingleSelection);   // 单选模式

    // 表头不可点击排序
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
    tableView->horizontalHeader()->setSectionsClickable(false);
#else
    tableView->horizontalHeader()->setClickable(false);
#endif

    // 编辑模式设置
    if (edit) {
        // 单击或双击进入编辑
        tableView->setEditTriggers(QAbstractItemView::CurrentChanged | QAbstractItemView::DoubleClicked);
    } else {
        // 禁止编辑
        tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    }
}

// 打开文件并弹出确认框
void QtHelper::openFile(const QString &fileName, const QString &msg)
{
#ifdef __arm__
    return;
#endif
    if (!QFile(fileName).exists()) {
        return;
    }
    // 确认后用系统默认程序打开
    if (QtHelper::showMessageBoxQuestion(msg + "成功, 确定现在就打开吗?") == QMessageBox::Yes) {
        QString url = QString("file:///%1").arg(fileName);
        QDesktopServices::openUrl(QUrl(url, QUrl::TolerantMode));
    }
}

// 检查ini配置文件是否完整
// 返回false表示需要重新生成配置文件
bool QtHelper::checkIniFile(const QString &iniFile)
{
    QFile file(iniFile);
    // 文件大小为0说明配置丢失
    if (file.size() == 0) {
        return false;
    }

    // 逐行检查每个key是否有value
    if (file.open(QFile::ReadOnly)) {
        bool ok = true;
        while (!file.atEnd()) {
            QString line = file.readLine();
            line.replace("\r", "");
            line.replace("\n", "");
            QStringList list = line.split("=");

            if (list.count() == 2) {
                QString key = list.at(0);
                QString value = list.at(1);
                // value为空说明配置不完整
                if (value.isEmpty()) {
                    qDebug() << TIMEMS << "ini node no value" << key;
                    ok = false;
                    break;
                }
            }
        }

        if (!ok) {
            return false;
        }
    } else {
        return false;
    }

    return true;
}

// 首尾截断字符串显示(用于文件名过长时的省略)
// left: 保留左侧字符数, right: 保留右侧字符数
// file: 是否去掉扩展名, mid: 省略号字符
QString QtHelper::cutString(const QString &text, int len, int left, int right, bool file, const QString &mid)
{
    QString result = text;
    // 文件名模式:先去掉扩展名
    if (file && result.contains(".")) {
        int index = result.lastIndexOf(".");
        result = result.mid(0, index);
    }

    // 超过长度时截断为: 前缀...后缀
    if (result.length() > len) {
        result = QString("%1%2%3").arg(result.left(left)).arg(mid).arg(result.right(right));
    }

    return result;
}

// ============================================================================
// 图片缩放/居中
// ============================================================================

// 根据图片尺寸和窗体区域计算居中缩放后的区域
// scaleMode: 0=自动调整(仅当图片大于容器时缩放), 1=等比缩放, 2=拉伸填充
QRect QtHelper::getCenterRect(const QSize &imageSize, const QRect &widgetRect, int borderWidth, int scaleMode)
{
    QSize newSize = imageSize;
    // 减去边框宽度得到有效区域
    QSize widgetSize = widgetRect.size() - QSize(borderWidth * 1, borderWidth * 1);

    if (scaleMode == 0) {
        // 仅当图片超出容器时才缩放
        if (newSize.width() > widgetSize.width() || newSize.height() > widgetSize.height()) {
            newSize.scale(widgetSize, Qt::KeepAspectRatio);
        }
    } else if (scaleMode == 1) {
        // 始终等比缩放
        newSize.scale(widgetSize, Qt::KeepAspectRatio);
    } else {
        // 拉伸填充(忽略宽高比)
        newSize = widgetSize;
    }

    // 计算居中坐标
    int x = widgetRect.center().x() - newSize.width() / 2;
    int y = widgetRect.center().y() - newSize.height() / 2;
    // 偏移1像素避免偶数坐标导致的渲染问题
    x += (x % 2 == 0 ? 1 : 0);
    y += (y % 2 == 0 ? 1 : 0);
    return QRect(x, y, newSize.width(), newSize.height());
}

// 缩放图片到指定尺寸
// fast: true=快速缩放(性能好), false=平滑缩放(质量高)
void QtHelper::getScaledImage(QImage &image, const QSize &widgetSize, int scaleMode, bool fast)
{
    Qt::TransformationMode mode = fast ? Qt::FastTransformation : Qt::SmoothTransformation;
    if (scaleMode == 0) {
        // 仅当图片超出容器时才缩放
        if (image.width() > widgetSize.width() || image.height() > widgetSize.height()) {
            image = image.scaled(widgetSize, Qt::KeepAspectRatio, mode);
        }
    } else if (scaleMode == 1) {
        // 始终等比缩放
        image = image.scaled(widgetSize, Qt::KeepAspectRatio, mode);
    } else {
        // 拉伸填充
        image = image.scaled(widgetSize, Qt::IgnoreAspectRatio, mode);
    }
}

// ============================================================================
// 时间/大小格式化
// ============================================================================

// 毫秒数转MM:SS格式字符串
QString QtHelper::getTimeString(qint64 time)
{
    time = time / 1000;
    // 分钟和秒数都补零到2位
    QString min = QString("%1").arg(time / 60, 2, 10, QChar('0'));
    QString sec = QString("%2").arg(time % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(min).arg(sec);
}

// QElapsedTimer转秒数字符串(保留3位小数)
QString QtHelper::getTimeString(QElapsedTimer timer)
{
    return QString::number((float)timer.elapsed() / 1000, 'f', 3);
}

// 文件大小转可读字符串(KB/MB/GB/TB)
QString QtHelper::getSizeString(quint64 size)
{
    float num = size;
    QStringList list;
    list << "KB" << "MB" << "GB" << "TB";

    QString unit("bytes");
    QStringListIterator i(list);
    // 循环除以1024直到小于1024
    while (num >= 1024.0 && i.hasNext()) {
        unit = i.next();
        num /= 1024.0;
    }

    return QString("%1 %2").arg(QString::number(num, 'f', 2)).arg(unit);
}

// ============================================================================
// 系统设置
// ============================================================================

// 设置系统日期时间
// Windows通过cmd命令设置,Linux通过date命令和hwclock同步硬件时钟
void QtHelper::setSystemDateTime(const QString &year, const QString &month, const QString &day, const QString &hour, const QString &min, const QString &sec)
{
#ifdef Q_OS_WIN
    QProcess p;
    // 先设置日期(date YYYY-MM-DD)
    p.start("cmd", QStringList());
    p.waitForStarted();
    p.write(QString("date %1-%2-%3\n").arg(year).arg(month).arg(day).toLatin1());
    p.closeWriteChannel();
    p.waitForFinished(1000);
    p.close();
    // 再设置时间(time HH:MM:SS.00)
    p.start("cmd", QStringList());
    p.waitForStarted();
    p.write(QString("time %1:%2:%3.00\n").arg(hour).arg(min).arg(sec).toLatin1());
    p.closeWriteChannel();
    p.waitForFinished(1000);
    p.close();
#else
    // Linux: date命令格式 MMDDHHmmYYYY.ss
    QString cmd = QString("date %1%2%3%4%5.%6").arg(month).arg(day).arg(hour).arg(min).arg(year).arg(sec);
    system(cmd.toLatin1());
    // 同步到硬件时钟
    system("hwclock -w");
#endif
}

// 设置开机自启动(当前程序)
void QtHelper::runWithSystem(bool autoRun)
{
    QtHelper::runWithSystem(qApp->applicationName(), qApp->applicationFilePath(), autoRun);
}

// 设置开机自启动(指定程序)
// 通过Windows注册表Run键实现
void QtHelper::runWithSystem(const QString &fileName, const QString &filePath, bool autoRun)
{
#ifdef Q_OS_WIN
    // 写入注册表Run键(autoRun=false时清空路径值)
    QSettings reg("HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    reg.setValue(fileName, autoRun ? QDir::toNativeSeparators(filePath) : "");
#endif
}

// 启动外部程序(已在运行则不重复启动)
void QtHelper::start(const QString &path, const QString &name, bool bin)
{
#ifdef Q_OS_WIN
    QString cmd1 = "tasklist";
    QString cmd2 = QString("%1/%2%3").arg(path).arg(name).arg(bin ? ".exe" : "");
#else
    QString cmd1 = "ps -aux";
    QString cmd2 = QString("%1/%2").arg(path).arg(name);
#endif

#ifndef Q_OS_WASM
    // 先检查进程是否已运行
    QProcess p;
    p.start(cmd1, QStringList());
    if (p.waitForFinished()) {
        QString result = p.readAll();
        if (result.contains(name)) {
            return; // 已在运行,跳过
        }
    }

    // 路径含空格时加引号(兼容Program Files等路径)
    if (cmd2.contains(" ")) {
        cmd2 = "\"" + cmd2 + "\"";
    }

    // 切换到程序所在目录后启动
    QDir::setCurrent(path);
    QProcess::startDetached(cmd2, QStringList());
    // 启动完成后切回默认目录
    QDir::setCurrent(QtHelper::appPath());
#endif
}
