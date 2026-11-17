// ============================================================================
// fire_guard.h - 动火监护规则引擎（纯逻辑，不依赖 Qt / OpenCV / RKNN）
// ============================================================================
//
// 语义：画面里出现明火/烟雾（动火作业）时，现场必须至少有两名人员
// （作业人 + 监火人）；人数不足持续超过宽限期即判违规。
//   - 只做通道级计数，不做人-火几何关联：动火场景没有稳定的"火源框"可依赖，
//     且现场机位下两个人通常都在明火附近；
//   - 人数按本帧人模型输出的框数统计，不做身份去重（检测框本身就是目标级）；
//   - 三态状态机 IDLE → WATCHING → ALARM，与登高守护同口径：
//     宽限期内补齐人数不报警，ALARM 后人数连续达标 clearSec 才消警；
//   - 明火消失后短暂保留状态 lostSec，避免单帧漏检把已累计的宽限计时清零。
// 时间由调用方喂单调秒值（steady_clock），本模块不读时钟，便于离线单测。
// ============================================================================
#ifndef FIRE_GUARD_H
#define FIRE_GUARD_H

#include <optional>
#include <vector>

namespace fireguard {

/// xyxy 像素框 + 置信度（坐标已还原到原图，y 轴向下）
struct Box {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float conf = 0;
};

/// 判定参数：人数门槛与三个时间常量
struct Params {
    int minPersons = 2;      ///< 动火现场最少在场人数（作业人 + 监火人）
    double graceSec = 2.0;   ///< 人数不足持续多久升 ALARM
    double clearSec = 1.0;   ///< 人数连续达标多久消警
    double lostSec = 1.0;    ///< 明火连续丢失多久销毁状态
};

enum class State { Idle, Watching, Alarm };

/// 本帧判定结果（通道级，最多一项）
struct GuardView {
    Box box;                  ///< 代表性明火框（面积最大者），用于抓拍/上报
    State state = State::Idle;
    bool enteredAlarm = false; ///< 本帧刚进入 ALARM（报警/抓拍的沿）
    int personCount = 0;       ///< 本帧检测到的人数
};

/// 跨帧维护通道级状态机；无明火时不输出任何 view
class Tracker {
public:
    explicit Tracker(const Params &p = Params());

    void setParams(const Params &p);
    const Params &params() const { return params_; }

    /// 推进一帧。now = 单调秒值。
    /// fires 为空表示本帧未检出明火/烟雾；persons 为本帧全部人框。
    /// 返回 0 或 1 项（通道级规则）。
    std::vector<GuardView> step(const std::vector<Box> &fires,
                                const std::vector<Box> &persons, double now);

    /// 清空状态（停用/重开流时调）
    void reset();

private:
    struct Guard {
        State state = State::Idle;
        Box box;
        int personCount = 0;
        std::optional<double> lowSince;    ///< 人数不足的起点
        std::optional<double> enoughSince; ///< ALARM 中人数达标的起点
        std::optional<double> lostSince;   ///< 明火消失的起点
        int alarmCount = 0;
    };

    Params params_;
    Guard guard_;
};

}  // namespace fireguard

#endif  // FIRE_GUARD_H
