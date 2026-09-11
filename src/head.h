// ============================================================================
// head.h - 全局头文件
// ============================================================================
//
// 本文件包含所有项目通用的 Qt 头文件。
// 在每个源文件中包含此文件，可以避免重复包含。
//
// 兼容性处理：
//   - Qt 5: 使用 QtWidgets 模块
//   - Qt 6: 使用 QtCore5Compat 模块（兼容 Qt 5 API）
//
// 编码设置：
//   - #pragma execution_character_set("utf-8"): 设置源文件编码为 UTF-8
//   - 确保中文字符串在编译时不乱码
//
// ============================================================================

#ifndef HEAD_H
#define HEAD_H

#include <QtCore>      // Qt 核心模块（QObject, QString, QVector 等）
#include <QtGui>       // Qt GUI 模块（QImage, QColor, QFont 等）

// Qt 5 需要单独包含 QtWidgets 模块
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
#include <QtWidgets>   // Qt 界面模块（QWidget, QMainWindow 等）
#endif

// Qt 6 需要包含 QtCore5Compat 以兼容 Qt 5 的 API
#if (QT_VERSION >= QT_VERSION_CHECK(6,0,0))
#include <QtCore5Compat>  // Qt 5 兼容模块（QStringRef 等）
#endif

// 设置源文件编码为 UTF-8
// 确保中文字符串（如 "检测"、"报警"）在编译时不乱码
#pragma execution_character_set("utf-8")

#endif // HEAD_H
