/*-------------------------------------------
                Includes
-------------------------------------------*/
// ============================================================================
// main.cpp - 程序入口点
// ============================================================================
//
// GUI 模式：./idge
//
// ============================================================================

#include <chrono>
#include <functional>
#include "runtime_paths.h"
#include "frmmain.h"
#include "rtsp_live_service.h"   // 进程退出前释放 RTSP 直播源
#include "appinit.h"
#include "qthelper.h"

#include "qdebug.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/signal.h>
#include <algorithm>
#include <map>
#include <thread>
#include <QTimer>
#include <QDateTime>
#include <QFileInfo>
#include <QDir>

#include "ConfigManager.h"
#include <QWidget>
#include <gelf.h>
#include <GL/gl.h>
#include "gl_video_widget.h"

#include "alarm_manager.h"
#include "http_file_service.h"   // 报警上行 url 模式的内嵌文件服务（进程退出时停在这里）
#include "database_manager.h"
#include "media_runtime.h"
#include "gb28181_manager.h"

// ============================================================================
// 信号处理 - 优雅关闭
// ============================================================================
// 处理 SIGTERM (kill) 和 SIGINT (Ctrl+C) 信号
// 收到信号后设置标志位，让主循环优雅退出
// ============================================================================
static volatile sig_atomic_t g_shutdownRequested = 0;

static void signalHandler(int signum)
{
    Q_UNUSED(signum);
    g_shutdownRequested = 1;
    qInfo() << "Shutdown signal received, cleaning up...";
}

// ============================================================================
// 主函数
// ============================================================================
// 启动 Qt 图形界面
// ============================================================================
int main(int argc, char *argv[])
{
    // 运行时数据路径一次性迁移（旧布局 → data/），必须先于一切文件打开
    RuntimePaths::migrateLegacy();

    // 设置 Qt 使用 EGL 渲染（硬件加速 OpenGL）
    // QT_XCB_GL_INTEGRATION=xcb_egl: 使用 EGL 后端，支持硬件加速
    qputenv("QT_XCB_GL_INTEGRATION", "xcb_egl");

    QApplication a(argc, argv);

    // ============================================================================
    // 配置 OpenGL ES 格式
    // ============================================================================
    // 使用 OpenGL ES 3.0（支持更多特性）
    // 双缓冲（避免画面撕裂）
    // 调试上下文（开发时启用，生产环境可关闭）
    // ============================================================================
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGLES);
    fmt.setVersion(3, 0);
    fmt.setSwapBehavior(QSurfaceFormat::DoubleBuffer);

    fmt.setOptions(QSurfaceFormat::DebugContext);  // 调试用
    QSurfaceFormat::setDefaultFormat(fmt);

    // 注册自定义类型（用于 Qt 信号/槽跨线程传递）
    qRegisterMetaType<RenderFrame>("RenderFrame");

    // 初始化 Qt 辅助工具
    QtHelper::initMain();
    QtHelper::initOpenGL(2, true, false);
    AppInit::Instance()->start();

    // 设置字体和编码
    QtHelper::setFont(16);  // 统一基础字号 16px（字阶：12角标/13提示/14导航/16正文/18卡片标题/22页面标题/30大数字）
    QtHelper::setCode();

    // Qt 自带控件的内置文案汉化（文件对话框的 Look in / File name / Files of type
    // 等标签、日期编辑器的"月/年"等），这些只能靠词典，代码里逐个改不现实。
    QtHelper::setTranslator(QStringLiteral(":/qm/qt_zh_CN.qm"));

    // 但消息框的标准按钮词典管不了：那份 .qm 里 QMessageBox 上下文只有裸 "OK"，
    // 而 Qt 源码取的是 "&OK" + 限定串 "QMessageBox"，对不上，实测装与不装按钮都是
    // OK/Cancel。所以再挂一个全局事件过滤器集中改写（见 qthelper）。
    QtHelper::installMessageBoxTextFilter();

    // 初始化数据库
    AlarmManager::instance().initDatabase(RuntimePaths::database());
    AlarmManager::instance().loadAlarmsFromDatabase();

    // 注册 FFmpeg 设备与网络组件（进程级一次）。智能识别页要开 /dev/videoN、
    // 国标推流要走 RTSP，都依赖 libavdevice/libavformat 的这一步注册。
    initializeMediaRuntime();

    // GB28181 国标推流：按需启动，判据是 Gb28181Config::load() 能否从配置里
    // 凑出可用的 platform + streams（gb28181 段和顶层 platform/streams 都认），
    // 而非"有没有 gb28181 键"。没有配置段就是正常态，load 返回 false 后静默跳过
    // （想确认是否启用可查 data/config.json 的 gb28181 段）；
    // 配置写错（serverHost 缺失、地址不是 IPv4 等）同样只是不启用，不影响主程序。
    // probe 只用于判据，真正的配置由 start() 自己再读一次。
    std::unique_ptr<Gb28181Manager> gb28181;
    {
        const QString configPath = RuntimePaths::configFile();
        Gb28181Config probe;
        QString probeError;
        if (Gb28181Config::load(configPath, &probe, &probeError)) {
            gb28181 = std::make_unique<Gb28181Manager>();
            if (!gb28181->start(configPath)) {
                qWarning() << "GB28181 manager failed to start";
                gb28181.reset();
            }
        }
    }

    // 注册信号处理（优雅关闭）
    signal(SIGTERM, signalHandler);  // kill 命令
    signal(SIGINT, signalHandler);   // Ctrl+C

    // 主窗口收进内层作用域：退出事件循环后先析构窗口（连带停掉 4 路解码线程），
    // 再进入程序末尾清理。这一段整体包在一个作用域里，不要往外挪。
    {
    // 创建并显示主窗口
    frmMain w;

    // 定时备份数据库（每天凌晨 3 点）
    // 使用 QTimer 定期检查是否需要备份
    QTimer backupTimer;
    QObject::connect(&backupTimer, &QTimer::timeout, []() {
        QDateTime now = QDateTime::currentDateTime();
        // 每天凌晨 3 点执行备份
        if (now.time().hour() == 3 && now.time().minute() == 0) {
            QString backupPath = RuntimePaths::backupFile(now.toString("yyyyMMdd"));
            if (DatabaseManager::instance().backup(backupPath)) {
                qInfo() << "Daily backup completed:" << backupPath;
            }
        }
    });
    backupTimer.start(60000);  // 每分钟检查一次

    // 启动时立即执行一次备份（确保有备份）
    {
        QString backupPath = RuntimePaths::backupFile(
            QDateTime::currentDateTime().toString("yyyyMMdd"));
        DatabaseManager::instance().backup(backupPath);
    }

    // 定时检查关闭信号（收到 SIGTERM/SIGINT 时优雅退出）
    QTimer shutdownCheckTimer;
    QObject::connect(&shutdownCheckTimer, &QTimer::timeout, []() {
        if (g_shutdownRequested) {
            qApp->quit();
        }
    });
    shutdownCheckTimer.start(500);  // 每 500ms 检查一次

    QtHelper::setFormInCenter(&w);
    // 十寸触屏要的是"整屏都是应用"，而 QtHelper::getScreenRect() 取的是
    // availableGeometry() —— 它天生排除 dock 的 strut，所以旧的"铺满可用区"
    // 永远盖不掉那条 dock。只有真全屏（_NET_WM_STATE_FULLSCREEN）会让 mutter
    // 自己收起 top-bar/dash（GNOME 的 dash 是 shell actor，层级在普通 X 窗之上，
    // 改 geometry / X11BypassWindowManagerHint 都动不了它）。
    // setFormInCenter 保留：万一 WM 拒绝了全屏请求，居中态仍是能用的兜底。
    w.showFullScreen();
    // 全屏时禁掉标题栏拖动（appinit 的全局事件过滤器按这个 property 搬窗口），
    // 口径与 frmMain::on_btnMenu_Max_clicked 保持一致：拖一次窗口就会把全屏态搅没。
    w.setProperty("canMove", !w.isFullScreen());

    // 进入 Qt 事件循环
    a.exec();
    }   // ~frmMain / ~PlayerWidget 在这里跑：4 路解码线程随之停干净

    // 程序退出前清理和同步
    qInfo() << "Shutting down, cleaning up...";
    if (gb28181) gb28181->stop();
    // 先释放 RTSP 直播源（mk_media_release），再停 ZLM 全部服务器；
    // 顺序反了会导致 mk_stop_all_server 已经把 media source 销毁、再 release 就 UAF。
    RtspLiveService::instance().stop();
    // 内嵌 http 文件服务（报警上行 url 模式）。停在这里而不是 ~frmMain：
    // mk_env_init 是进程级一次、mk_stop_all_server 之后不能再起，
    // 生命周期必须覆盖整个进程，而 frmMain 是 main 的栈对象、析构点比这早
    HttpFileService::instance().stop();
    AlarmManager::instance().syncToDatabase();
    AlarmManager::instance().cleanOldData(30);

    return 0;
}
