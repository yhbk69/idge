#include "appinit.h"
#include "qmutex.h"
#include "qapplication.h"
#include "qevent.h"
#include "qwidget.h"
#include "qdebug.h"

// ============================================================================
// 单例实例化
// ============================================================================
SINGLETON_IMPL(AppInit)

// ============================================================================
// 构造函数
// ============================================================================
AppInit::AppInit(QObject *parent) : QObject(parent)
{
}

// ============================================================================
// eventFilter - 全局事件过滤器
// 拦截鼠标事件，实现窗口拖动功能
// 当窗口设置了"canMove"属性时启用拖动
// ============================================================================
bool AppInit::eventFilter(QObject *watched, QEvent *event)
{ 
    QWidget *w = (QWidget *)watched;

    // 检查窗口是否支持拖动（需要设置"canMove"属性）
    if (!w->property("canMove").toBool()) {
        return QObject::eventFilter(watched, event);
    }

    static QPoint mousePoint;     // 鼠标相对于窗口的偏移量
    static bool mousePressed = false;  // 鼠标左键是否按下

    int type = event->type();
    QMouseEvent *mouseEvent = static_cast<QMouseEvent *>(event);

    if (type == QEvent::MouseButtonPress) {
        // 鼠标按下：记录偏移量
        if (mouseEvent->button() == Qt::LeftButton) {
            mousePressed = true;
            mousePoint = mouseEvent->globalPos() - w->pos();
        }
    } else if (type == QEvent::MouseButtonRelease) {
        // 鼠标释放：清除按下状态
        mousePressed = false;
    } else if (type == QEvent::MouseMove) {
        // 鼠标移动：实时更新窗口位置（实现拖动效果）
        if (mousePressed) {
            w->move(mouseEvent->globalPos() - mousePoint);
            return true;  // 事件已处理，不再传递
        }
    }

    return QObject::eventFilter(watched, event);
}

// ============================================================================
// start - 启动应用初始化
// 将自身安装为QApplication的全局事件过滤器
// ============================================================================
void AppInit::start()
{
    qApp->installEventFilter(this);
}
