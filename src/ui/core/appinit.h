#ifndef APPINIT_H
#define APPINIT_H

// ============================================================================
// 应用初始化类头文件
// 提供应用程序启动时的全局配置和事件处理
// 支持窗口拖动功能（通过Qt事件过滤器实现）
// ============================================================================

#include <QObject>
#include "singleton.h"

// ============================================================================
// AppInit - 应用初始化类（单例模式）
// 继承QObject以支持Qt事件系统
// ============================================================================
class AppInit : public QObject
{
    Q_OBJECT SINGLETON_DECL(AppInit)
public:
    explicit AppInit(QObject *parent = 0);

protected:
    /**
     * @brief 事件过滤器
     * 拦截并处理全局事件，实现窗口拖动等功能
     */
    bool eventFilter(QObject *watched, QEvent *event);

public slots:
    /**
     * @brief 启动应用初始化
     * 安装全局事件过滤器
     */
    void start();
};

#endif // APPINIT_H
