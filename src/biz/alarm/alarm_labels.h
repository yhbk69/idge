#pragma once

// ============================================================================
// alarm_labels.h — 报警类型标签与规则名的唯一事实源
// ============================================================================
//
// 【所属模块】src/biz/alarm。
// 为什么放在 alarm 而不是留在 pad：v1 §13.2.1 要求 `type`/`typeLabel`/
// `categoryLabel` 与 `rule` 都由边缘设备下发，Pad 不做本地映射；而 `rule` 现在
// 要**落库**（alarms.rule）并在本机详情框显示，产生方是 biz/alarm。两处各自维护
// 一张类别表必然分叉，所以表只留这一份，Pad 出门口与报警打戳都读这里。
//
// 【依赖方向】biz/pad → biz/alarm 是既有方向（pad_wire 本来就吃 AlarmRecord），
// 本文件不依赖 pad 的任何东西。
//
// ============================================================================

#include <QString>
#include <QVector>

namespace alarm_labels {
// 命名空间刻意不叫 alarm：unistd.h 里有 `unsigned int alarm(unsigned int)`，
// 包含链带进它的编译单元（详情框 → ffmpeg_video_decoder.h）会直接报
// "redeclared as different kind of entity"。

/// §13.2.1 的类型三元组。
struct TypeWire {
    QString code;            ///< 例 HELMET / INTRUSION
    QString typeLabel;       ///< 例 未戴安全帽
    QString categoryLabel;   ///< 例 PPE违规
};

/**
 * @brief 报警类型三元组（围栏按 alarmType，检测按 className）
 * @param alarmType 库里的 alarm_type：detection / fence_in / fence_out / 老值 fence
 * @param className 库里的 class_name（labels.txt 行原文）
 */
TypeWire typeWireOf(const QString &alarmType, const QString &className);

/**
 * @brief §13.2.1 的 `rule`：触发的检测规则名称
 * @note 规约给的例子是"安全帽检测 v2.4"这种带版本的人话；版本已经各有专列
 *       （modelVersion / fenceVersion），这里只给**规则名**，不重复拼版本。
 */
QString ruleNameOf(const QString &alarmType, const QString &className);

/// v1 §4.1 的处置选项。id 就是落进 alarms.dispose_option_id 的那个数。
struct DisposeOption {
    int id = 0;
    QString label;
};

/**
 * @brief 内置处置选项（唯一一份）：Pad 的 GET /api/alarms/{id}/dispose-options
 *        与本机详情框的处置单选组共用这张表。
 * @note 2026-11-06 定的四条，现场后续会改措辞——改这一处两端一起变；
 *       库里存的是 id 不是中文，所以改文案不会让历史统计对不上。
 *       config 的 pad_http/dispose_options 仍可整组覆盖 Pad 那一侧（§4.1 的既有口径），
 *       但本机详情框读的是这张内置表：两端选项不一致时 Pad 提交的文本
 *       可能不在本机列表里，那时 dispose_option_id 记 0（= 没选内置项），不硬套。
 */
QVector<DisposeOption> disposeOptions();

}  // namespace alarm_labels
