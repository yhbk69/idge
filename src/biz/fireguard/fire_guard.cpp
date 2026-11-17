// ============================================================================
// fire_guard.cpp - 动火监护规则引擎实现
// ============================================================================
#include "fire_guard.h"

#include <algorithm>

namespace fireguard {

namespace {

float area(const Box &b)
{
    return std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
}

/// 多簇明火取面积最大的一框作为代表：报警截图/上报要落在最明显的一处火源上
Box representativeFire(const std::vector<Box> &fires)
{
    return *std::max_element(fires.begin(), fires.end(),
                             [](const Box &a, const Box &b) { return area(a) < area(b); });
}

}  // namespace

Tracker::Tracker(const Params &p) : params_(p) {}

void Tracker::setParams(const Params &p)
{
    params_ = p;
}

void Tracker::reset()
{
    guard_ = Guard{};
}

std::vector<GuardView> Tracker::step(const std::vector<Box> &fires,
                                     const std::vector<Box> &persons, double now)
{
    Guard &g = guard_;

    // ---- 明火缺失：短暂保留状态，超过 lostSec 才回 IDLE ----
    if (fires.empty()) {
        if (g.state == State::Idle)
            return {};
        if (!g.lostSince)
            g.lostSince = now;
        else if (now - *g.lostSince >= params_.lostSec)
            g = Guard{};
        return {};
    }
    g.lostSince.reset();

    const int count = (int)persons.size();
    const bool enough = count >= params_.minPersons;
    const int alarmsBefore = g.alarmCount;

    if (g.state == State::Idle) {
        g.state = State::Watching;
        g.lowSince = enough ? std::nullopt : std::optional<double>(now);
        g.enoughSince.reset();
    } else if (g.state == State::Watching) {
        if (enough) {
            g.lowSince.reset();
        } else {
            if (!g.lowSince)
                g.lowSince = now;
            else if (now - *g.lowSince >= params_.graceSec) {
                g.state = State::Alarm;
                ++g.alarmCount;
                g.enoughSince.reset();
            }
        }
    } else {  // Alarm
        if (!enough) {
            g.enoughSince.reset();
        } else if (!g.enoughSince) {
            g.enoughSince = now;
        } else if (now - *g.enoughSince >= params_.clearSec) {
            g.state = State::Watching;
            g.lowSince.reset();
            g.enoughSince.reset();
        }
    }

    GuardView view;
    view.box = representativeFire(fires);
    view.state = g.state;
    view.personCount = count;
    // 只有"本帧刚升 Alarm"才是新事件：alarmCount 在本帧自增过一次。
    view.enteredAlarm = g.alarmCount > alarmsBefore;
    return {view};
}

}  // namespace fireguard
