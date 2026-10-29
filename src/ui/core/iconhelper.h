#ifndef ICONHELPER_H
#define ICONHELPER_H

// ============================================================================
// 图标字体辅助类头文件
// 提供图形字体图标管理功能，支持多种图标字体库
// 包括：阿里巴巴图标字体、FontAwesome、天气图标字体等
// 作者: feiyangqingyun(QQ:517216493) 2016-11-23
//
// 主要功能：
// 1. 支持多种图形字体文件，一个类通用所有图形字体
// 2. 可设置QLabel、QAbstractButton文本为图形字体
// 3. 可将图形字体转换为按钮图标
// 4. 内置导航栏样式设置功能
// 5. 支持正常/悬停/按下/选中四种状态的颜色切换
// ============================================================================

#include <QtGui>
#if (QT_VERSION >= QT_VERSION_CHECK(5,0,0))
#include <QtWidgets>
#endif

#ifdef quc
class Q_DECL_EXPORT IconHelper : public QObject
#else
class IconHelper : public QObject
#endif

{
    Q_OBJECT

private:
    // 各图形字体类的静态实例
    static IconHelper *iconFontAliBaBa;      // 阿里巴巴图标字体
    static IconHelper *iconFontAwesome;      // FontAwesome图标字体
    static IconHelper *iconFontAwesome6;     // FontAwesome 6图标字体
    static IconHelper *iconFontWeather;      // 天气图标字体
    static int iconFontIndex;                // 当前使用的字体索引

public:
    // ============================================================================
    // StyleColor - 导航栏样式配置结构体
    // 包含位置、尺寸、颜色等完整的样式参数
    // ============================================================================
    struct StyleColor {
        QString position;           // 图标位置：left/right/top/bottom
        bool defaultBorder;         // 是否默认显示边框

        quint32 btnWidth;           // 按钮宽度
        quint32 btnHeight;          // 按钮高度

        quint32 iconSize;           // 图标字体渲染大小
        quint32 iconWidth;          // 图标图片宽度
        quint32 iconHeight;         // 图标图片高度

        quint32 borderWidth;        // 边框宽度
        QString borderColor;        // 边框颜色

        QString normalBgColor;      // 正常状态背景颜色
        QString normalTextColor;    // 正常状态文字颜色
        QString hoverBgColor;       // 悬停状态背景颜色
        QString hoverTextColor;     // 悬停状态文字颜色
        QString pressedBgColor;     // 按下状态背景颜色
        QString pressedTextColor;   // 按下状态文字颜色
        QString checkedBgColor;     // 选中状态背景颜色
        QString checkedTextColor;   // 选中状态文字颜色

        // 默认构造函数：初始化为深色主题
        StyleColor() {
            position = "left";
            defaultBorder = false;

            btnWidth = 0;
            btnHeight = 0;

            iconSize = 12;
            iconWidth = 15;
            iconHeight = 15;

            borderWidth = 3;
            borderColor = "#029FEA";

            normalBgColor = "#292F38";
            normalTextColor = "#54626F";
            hoverBgColor = "#40444D";
            hoverTextColor = "#FDFDFD";
            pressedBgColor = "#404244";
            pressedTextColor = "#FDFDFD";
            checkedBgColor = "#44494F";
            checkedTextColor = "#FDFDFD";
        }

        // 设置常规颜色：普通状态 + 加深状态（悬停/按下/选中共用）
        void setColor(const QString &normalBgColor,
                      const QString &normalTextColor,
                      const QString &darkBgColor,
                      const QString &darkTextColor) {
            this->normalBgColor = normalBgColor;
            this->normalTextColor = normalTextColor;
            this->hoverBgColor = darkBgColor;
            this->hoverTextColor = darkTextColor;
            this->pressedBgColor = darkBgColor;
            this->pressedTextColor = darkTextColor;
            this->checkedBgColor = darkBgColor;
            this->checkedTextColor = darkTextColor;
        }
    };

    // ============================================================================
    // 静态接口方法
    // ============================================================================

    // 初始化所有图形字体（懒加载模式，仅首次调用时加载）
    static void initFont();
    // 设置图形字体索引（用于指定使用哪种字体）
    static void setIconFontIndex(int index);

    // 获取各图形字体的QFont对象
    static QFont getIconFontAliBaBa();
    static QFont getIconFontAwesome();
    static QFont getIconFontAwesome6();
    static QFont getIconFontWeather();

    // 根据图标值自动选择对应的字体类
    static IconHelper *getIconHelper(int icon);

    // 设置图形字体到QLabel/QAbstractButton（作为文本显示）
    static void setIcon(QLabel *lab, int icon, quint32 size = 12);
    static void setIcon(QAbstractButton *btn, int icon, quint32 size = 12);

    // 设置图形字体到按钮图标（转换为QPixmap）
    static void setPixmap(QAbstractButton *btn, const QColor &color,
                          int icon, quint32 size = 12,
                          quint32 width = 15, quint32 height = 15,
                          int flags = Qt::AlignCenter);
    // 获取图形字体转换的QPixmap图片
    static QPixmap getPixmap(const QColor &color, int icon, quint32 size = 12,
                             quint32 width = 15, quint32 height = 15,
                             int flags = Qt::AlignCenter);

    // 设置导航栏样式（支持QPushButton/QToolButton/QAbstractButton）
    static void setStyle(QWidget *widget, QList<QPushButton *> btns, QList<int> icons, const StyleColor &styleColor);
    static void setStyle(QWidget *widget, QList<QToolButton *> btns, QList<int> icons, const StyleColor &styleColor);
    static void setStyle(QWidget *widget, QList<QAbstractButton *> btns, QList<int> icons, const StyleColor &styleColor);

    // 构造函数：加载指定的字体文件
    explicit IconHelper(const QString &fontFile, const QString &fontName, QObject *parent = 0);

protected:
    // 事件过滤器：处理按钮的悬停、按下、选中等状态变化
    bool eventFilter(QObject *watched, QEvent *event);

private:
    QFont iconFont;                 // 当前使用的图形字体
    QList<QAbstractButton *> btns;  // 关联的按钮队列
    QList<QPixmap> pixNormal;       // 正常状态图标队列
    QList<QPixmap> pixHover;        // 悬停状态图标队列
    QList<QPixmap> pixPressed;      // 按下状态图标队列
    QList<QPixmap> pixChecked;      // 选中状态图标队列

private slots:
    // 按钮选中状态切换时更新图标
    void toggled(bool checked);

public:
    // 实例接口方法
    QFont getIconFont();
    void setIcon1(QLabel *lab, int icon, quint32 size = 12);
    void setIcon1(QAbstractButton *btn, int icon, quint32 size = 12);
    void setPixmap1(QAbstractButton *btn, const QColor &color,
                    int icon, quint32 size = 12,
                    quint32 width = 15, quint32 height = 15,
                    int flags = Qt::AlignCenter);
    QPixmap getPixmap1(const QColor &color, int icon, quint32 size = 12,
                       quint32 width = 15, quint32 height = 15,
                       int flags = Qt::AlignCenter);
    void setStyle1(QWidget *widget, QList<QPushButton *> btns, QList<int> icons, const IconHelper::StyleColor &styleColor);
    void setStyle1(QWidget *widget, QList<QToolButton *> btns, QList<int> icons, const IconHelper::StyleColor &styleColor);
    void setStyle1(QWidget *widget, QList<QAbstractButton *> btns, QList<int> icons, const IconHelper::StyleColor &styleColor);
};

#endif // ICONHELPER_H
