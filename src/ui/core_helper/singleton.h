#ifndef SINGLETON_H
#define SINGLETON_H

// ============================================================================
// 单例模式模板头文件
// 提供线程安全的单例实现宏
// 使用双重检查锁定(Double-Checked Locking)模式确保线程安全
// ============================================================================

#include <QScopedPointer>
#include <QMutex>

// ============================================================================
// SINGLETON_DECL - 单例声明宏
// 在类的public部分使用，声明静态的Instance()方法和self成员
// 同时禁用拷贝构造和赋值操作符
// ============================================================================
#define SINGLETON_DECL(Class) \
    public: \
        static Class *Instance(); \
    private: \
        Q_DISABLE_COPY(Class) \
        static QScopedPointer<Class> self;

// ============================================================================
// SINGLETON_IMPL - 单例实现宏
// 实现线程安全的单例创建：
// 1. 第一次检查：快速路径，避免每次调用都加锁
// 2. 加锁：仅在实例未创建时加锁
// 3. 第二次检查：在锁内再次检查，防止多线程重复创建
// ============================================================================
#define SINGLETON_IMPL(Class) \
    QScopedPointer<Class> Class::self; \
    Class *Class::Instance() { \
        if (self.isNull()) { \
            static QMutex mutex; \
            QMutexLocker locker(&mutex); \
            if (self.isNull()) { \
                self.reset(new Class); \
            } \
        } \
        return self.data(); \
    }

#endif // SINGLETON_H
