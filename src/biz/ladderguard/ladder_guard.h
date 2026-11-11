// ============================================================================
// ladder_guard.h - 登高守护规则引擎（纯逻辑，不依赖 Qt / OpenCV / RKNN）
// ============================================================================
//
// 语义：有人在梯子上（攀爬者）且梯下无人看护（扶梯人）= 违规。
// 移植自 detect_ladderguard.py / 登高守护实现方案.md（docs/ladderguard/）：
//   - 几何阈值全部取梯子框自身 W/H 的比例，与分辨率/机位/梯距无关；
//   - 先判攀爬者、剩下的人再判扶梯人（攀爬者绝不能自己当自己的扶梯人）；
//   - 梯子重复框（整梯框+上半段子框）先聚合成并集框，子框底边不是真梯底；
//   - 每把梯子一个三态状态机（IDLE/CLIMBING/ALARM），跨帧 IoU 跟踪梯子身份，
//     grace 宽限防闪报、clear 连续在场防误消警、丢失超时销毁。
// 时间由调用方喂单调秒值（steady_clock），本模块不读时钟，便于离线单测。
// ============================================================================
#ifndef LADDER_GUARD_H
#define LADDER_GUARD_H

#include <optional>
#include <vector>

namespace ladderguard {

/// xyxy 像素框 + 置信度（坐标已还原到原图，y 轴向下）
struct Box {
    float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    float conf = 0;
};

/// 几何比例与时间常量，默认值与参考实现一致
struct Params {
    float climberDy = 0.20f;  ///< 攀爬者：脚高于梯底 ≥ 0.2H 才算离地
    float climberDx = 0.20f;  ///< 攀爬者：人框水平中心允许外扩梯框 ±0.2W
    float spotterDy = 0.15f;  ///< 扶梯人：脚与梯底基线容差 ±0.15H
    float spotterDx = 0.80f;  ///< 扶梯人：人框中心到梯框最近边水平距 ≤0.8W
    float iouMatch = 0.30f;   ///< 跨帧匹配同一把梯子的 IoU 下限
    double graceSec = 2.0;    ///< 无扶梯人持续多久升 ALARM
    double clearSec = 1.0;    ///< 扶梯人连续在场多久降回 CLIMBING
    double lostSec = 1.0;     ///< 梯子连续丢失多久销毁状态机
};

enum class State { Idle, Climbing, Alarm };

/// 本帧某把梯子的判定结果
struct GuardView {
    int id = 0;
    Box box;                  ///< 本帧并集梯框
    State state = State::Idle;
    bool enteredAlarm = false; ///< 本帧刚进入 ALARM（报警/抓拍的沿）
    std::vector<Box> climbers;
    std::vector<Box> spotters;
};

float iou(const Box &a, const Box &b);

/// 重叠/包含的重复梯框聚合成并集框（conf 取簇内最大），避免子框底边造成假违规
std::vector<Box> mergeLadders(const std::vector<Box> &boxes);

/// 针对一把梯子把人框分成攀爬者/扶梯人；路人不出现在任何输出里
void classifyPersons(const Box &ladder, const std::vector<Box> &persons,
                     const Params &p, std::vector<Box> &climbers,
                     std::vector<Box> &spotters);

/// 跨帧维护「梯框 -> 状态机」映射；多梯独立计时独立告警
class Tracker {
public:
    explicit Tracker(const Params &p = Params());

    void setParams(const Params &p);
    const Params &params() const { return params_; }

    /// 推进一帧。now = 单调秒值；ladders 应为 mergeLadders() 之后的框。
    /// 返回本帧在场的每把梯子一项；无梯子返回空（守护不激活）。
    std::vector<GuardView> step(const std::vector<Box> &ladders,
                                const std::vector<Box> &persons, double now);

    /// 清空全部状态机（停用/重开流时调）。id 计数不复零：报警去重键含 guardId，
    /// 复零会让新梯子在节流窗口内撞上旧报警的键被静默吞掉。
    void reset();

private:
    struct Guard {
        int id = 0;
        Box box;
        State state = State::Idle;
        std::optional<double> noSpotterSince;  ///< CLIMBING：本轮无扶梯人起点
        std::optional<double> spotterSince;    ///< ALARM：本轮扶梯人连续在场起点
        std::optional<double> lostSince;       ///< 跨帧未匹配起点
        int alarmCount = 0;
    };

    Params params_;
    std::vector<Guard> guards_;
    int nextId_ = 0;
};

}  // namespace ladderguard

#endif  // LADDER_GUARD_H