// ============================================================================
// alarm_labels.cpp — 类别→类型三元组 / 规则名
// ============================================================================
// 表体从 src/biz/pad/pad_wire.cpp 的 typeOf() 原样搬来（含踩坑记录），搬完后
// Pad 出门口与报警打戳共用这一份。改这张表就是改 §13.2.1 的对外口径。
// ============================================================================
#include "alarm_labels.h"

namespace alarm_labels {

TypeWire typeWireOf(const QString &alarmType, const QString &className)
{
    // 围栏报警按 alarmType 定（角色决定语义，与检出什么类别无关），判据用**前缀**而不是精确匹配。
    // 库里有三代围栏值：实测 build 目录那份 `data/idge.db` = fence 25862 / fence_in 1739 /
    // fence_out 11（老行占 67%），仓库种子那份 = fence 933 / fence_in 0 / fence_out 0。
    // 精确匹配只认后两代，那两万多行老 `fence` 会落到"按类别名兜底"那一档——它们的
    // class_name 是 person/people，经 sync_result 出去就是 `type="PERSON"`，而 v1 §3.6
    // 明确"Pad 不做本地类型映射"，于是**围栏报警在平板列表里显示成一个人**。
    // 前缀口径与 alarm_dao.cpp 的 recordFromQuery、alarm_list_widget 的围栏表一致。
    if (alarmType.startsWith(QLatin1String("fence"))) {
        if (alarmType == QLatin1String("fence_out")) {
            // ⚠ v1 的 CROSS（越界）与本机"离岗"是否同一件事，规约没定义（歧义 A9）。
            //   这里按"人离开所有岗区域 = 越出授权范围"取 CROSS，等接口方确认后只改这一行。
            return {QStringLiteral("CROSS"), QStringLiteral("离岗"), QStringLiteral("区域违规")};
        }
        // 老行没有角色信息（那时代码里只有单一 `fence`，禁区与岗区域共用），
        // 所以离岗事件也会被一并归成闯入——这是**替历史做的决定**，不是漏判：
        // 新行从今往后都带 fence_in/fence_out 两代值，只有存量这批改不了。
        return {QStringLiteral("INTRUSION"), QStringLiteral("区域闯入"), QStringLiteral("区域违规")};
    }

    // 检测报警按类别名（labels.txt 行内容，各模型自定，故小写原样比对）。
    // 表里**只列违规类**：`yolo11n-ppe` 的五类是 un_helmet/helmet/people/vest/workWear，
    // 其中 helmet/vest/workWear 是"戴了帽/穿了衣"的合规结果，曾经也被写成违规标签
    // （helmet→"未戴安全帽"），设置页把某个模型的报警类别改成它们就会产出**方向相反的假报警**。
    // 合规类一律走下面的兜底（大写 code + 原名 + "其他"），等 A6 的类型取值表下来再逐条定。
    static const struct {
        const char *cls;
        const char *code;
        const char *typeLabel;
        const char *categoryLabel;
    } kTable[] = {
        {"un_helmet", "HELMET",  "未戴安全帽",  "PPE违规"},
        {"cigarette", "SMOKING", "吸烟",        "现场隐患"},
        {"smoking",   "SMOKING", "吸烟",        "现场隐患"},
        {"fire",      "FIRE",    "明火",        "现场隐患"},
        {"smoke",     "SMOKE",   "烟雾",        "现场隐患"},
    };
    for (const auto &row : kTable) {
        if (className == QLatin1String(row.cls)) {
            return {QString::fromUtf8(row.code), QString::fromUtf8(row.typeLabel),
                    QString::fromUtf8(row.categoryLabel)};
        }
    }

    // 兜底：v1 说"Pad 不做本地类型映射"，所以标签必须由这边给，不能空着。
    // 取不到就把类别名原样抬成大写 code + 同名标签，对端至少能显示"是什么检出的"。
    TypeWire fallback;
    fallback.code = className.isEmpty() ? QStringLiteral("UNKNOWN") : className.toUpper();
    fallback.typeLabel = className.isEmpty() ? QStringLiteral("未知报警") : className;
    fallback.categoryLabel = QStringLiteral("其他");
    return fallback;
}

QString ruleNameOf(const QString &alarmType, const QString &className)
{
    if (alarmType == QLatin1String("fence_out"))
        return QStringLiteral("离岗检测（在岗区域外）");
    if (alarmType.startsWith(QLatin1String("fence")))
        return QStringLiteral("禁区闯入检测");
    // 检测类：用同一张表的中文名，未登记的类别就报 labels.txt 原文，不编一个假中文
    const TypeWire wire = typeWireOf(alarmType, className);
    const bool known = wire.code != QLatin1String("UNKNOWN")
                       && wire.typeLabel != className;
    return known ? (wire.typeLabel + QStringLiteral("检测"))
                 : (className + QStringLiteral(" 检测"));
}

QVector<DisposeOption> disposeOptions()
{
    // id 一旦发出去就不能改含义（库里存的就是这个数），措辞可以换、id 只能往后加。
    static const struct { int id; const char *label; } kOptions[] = {
        {1, "已现场纠正"},
        {2, "已通知作业人员"},
        {3, "需停工整改"},
        {4, "无法现场处置"},
    };
    QVector<DisposeOption> out;
    out.reserve(int(sizeof(kOptions) / sizeof(kOptions[0])));
    for (const auto &one : kOptions)
        out.append(DisposeOption{one.id, QString::fromUtf8(one.label)});
    return out;
}

QString disposeOptionLabel(int id)
{
    for (const DisposeOption &one : disposeOptions()) {
        if (one.id == id)
            return one.label;
    }
    return QString();
}

QVector<QString> disposeOptionAliases(int id)
{
    // 只登记有出处的措辞：plan/edge-pad-v1.md §4.1 的示例把 id=2 写成"已提醒作业人员"，
    // 而本机出厂 label 是"已通知作业人员"（2026-11-06 现场定的四条之一）。
    // 对端照文档示例发过来的文本就是这个，认不出来等于这一列白存。
    if (id == 2)
        return {QStringLiteral("已提醒作业人员")};
    return {};
}

}  // namespace alarm_labels
