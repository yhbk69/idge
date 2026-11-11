// ============================================================================
// ladder_guard.cpp - 登高守护规则引擎实现
// ============================================================================
#include "ladder_guard.h"

#include <algorithm>
#include <cmath>

namespace ladderguard {

namespace {

float intersection(const Box &a, const Box &b)
{
    const float ix1 = std::max(a.x1, b.x1);
    const float iy1 = std::max(a.y1, b.y1);
    const float ix2 = std::min(a.x2, b.x2);
    const float iy2 = std::min(a.y2, b.y2);
    return std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
}

float area(const Box &b)
{
    return std::max(0.0f, b.x2 - b.x1) * std::max(0.0f, b.y2 - b.y1);
}

Box unionBox(const Box &a, const Box &b)
{
    return Box{std::min(a.x1, b.x1), std::min(a.y1, b.y1),
               std::max(a.x2, b.x2), std::max(a.y2, b.y2),
               std::max(a.conf, b.conf)};
}

}  // namespace

float iou(const Box &a, const Box &b)
{
    const float inter = intersection(a, b);
    if (inter <= 0)
        return 0.0f;
    return inter / (area(a) + area(b) - inter);
}

std::vector<Box> mergeLadders(const std::vector<Box> &boxes)
{
    std::vector<Box> items = boxes;
    std::stable_sort(items.begin(), items.end(),
                     [](const Box &a, const Box &b) { return a.conf > b.conf; });

    // 子框被父框包含时 IoU 可能很低（细长框尤其明显），补一层
    // 「交集 / 较小框面积」的包含判断，否则子框底边会凭空多出违规告警。
    bool merged = true;
    while (merged && items.size() > 1) {
        merged = false;
        for (size_t i = 0; i < items.size() && !merged; ++i) {
            for (size_t j = i + 1; j < items.size(); ++j) {
                const float inter = intersection(items[i], items[j]);
                if (inter <= 0)
                    continue;
                const float smaller = std::max(std::min(area(items[i]), area(items[j])), 1e-6f);
                if (iou(items[i], items[j]) >= 0.1f || inter / smaller >= 0.5f) {
                    items[i] = unionBox(items[i], items[j]);
                    items.erase(items.begin() + (long)j);
                    merged = true;
                    break;
                }
            }
        }
    }
    return items;
}

void classifyPersons(const Box &ladder, const std::vector<Box> &persons,
                     const Params &p, std::vector<Box> &climbers,
                     std::vector<Box> &spotters)
{
    climbers.clear();
    spotters.clear();
    const float w = ladder.x2 - ladder.x1;
    const float h = ladder.y2 - ladder.y1;
    if (w <= 1 || h <= 1)  // 退化框：避免除零与比例失真
        return;

    // ① 攀爬者：水平中心在梯框附近 + 脚明显高于梯底（y 向下，离地即坐标更小）
    std::vector<Box> others;
    for (const Box &person : persons) {
        const float cx = (person.x1 + person.x2) / 2.0f;
        const float feet = person.y2;
        const bool inX = cx >= ladder.x1 - p.climberDx * w && cx <= ladder.x2 + p.climberDx * w;
        const bool offGround = feet <= ladder.y2 - p.climberDy * h;
        if (inX && offGround)
            climbers.push_back(person);
        else
            others.push_back(person);
    }

    // ② 扶梯人：只有非攀爬者才有资格（否则梯上的人自己当自己的看护，违规永远报不出）
    for (const Box &person : others) {
        const float cx = (person.x1 + person.x2) / 2.0f;
        const float feet = person.y2;
        const float dx = std::max(std::max(ladder.x1 - cx, cx - ladder.x2), 0.0f);
        const bool nearBase = std::fabs(feet - ladder.y2) <= p.spotterDy * h;
        const bool nearLadder = dx <= p.spotterDx * w;
        if (nearBase && nearLadder)
            spotters.push_back(person);
    }
}

Tracker::Tracker(const Params &p) : params_(p) {}

void Tracker::setParams(const Params &p)
{
    params_ = p;
}

void Tracker::reset()
{
    guards_.clear();
}

std::vector<GuardView> Tracker::step(const std::vector<Box> &ladders,
                                     const std::vector<Box> &persons, double now)
{
    std::vector<GuardView> views;
    views.reserve(ladders.size());
    std::vector<char> matched(guards_.size(), 0);

    for (const Box &lbox : ladders) {
        // 贪心取尚未被本帧占用的状态机里 IoU 最大者作为同一把梯子
        int bestIdx = -1;
        float bestIou = params_.iouMatch;
        for (size_t t = 0; t < guards_.size(); ++t) {
            if (matched[t])
                continue;
            const float v = iou(lbox, guards_[t].box);
            if (v > bestIou) {
                bestIou = v;
                bestIdx = (int)t;
            }
        }
        if (bestIdx < 0) {
            Guard g;
            g.id = ++nextId_;
            g.box = lbox;
            guards_.push_back(g);
            matched.push_back(0);  // 与 guards_ 同步增长，否则下面 matched[bestIdx] 越界写
            bestIdx = (int)guards_.size() - 1;
        }
        matched[bestIdx] = 1;
        Guard &g = guards_[bestIdx];

        const int alarmsBefore = g.alarmCount;
        g.box = lbox;
        g.lostSince.reset();
        GuardView view;
        view.id = g.id;
        view.box = lbox;
        classifyPersons(lbox, persons, params_, view.climbers, view.spotters);
        const bool hasClimber = !view.climbers.empty();
        const bool hasSpotter = !view.spotters.empty();

        if (g.state == State::Idle) {
            if (hasClimber) {
                g.state = State::Climbing;
                g.noSpotterSince = hasSpotter ? std::nullopt : std::optional<double>(now);
            }
        } else if (g.state == State::Climbing) {
            if (!hasClimber) {
                g.state = State::Idle;
                g.noSpotterSince.reset();
            } else if (hasSpotter) {
                g.noSpotterSince.reset();
            } else {
                if (!g.noSpotterSince)
                    g.noSpotterSince = now;
                else if (now - *g.noSpotterSince >= params_.graceSec) {
                    g.state = State::Alarm;
                    g.alarmCount++;
                    g.spotterSince.reset();
                }
            }
        } else {  // Alarm
            if (!hasClimber) {
                g.state = State::Idle;
                g.spotterSince.reset();
            } else if (hasSpotter) {
                if (!g.spotterSince)
                    g.spotterSince = now;
                else if (now - *g.spotterSince >= params_.clearSec) {
                    g.state = State::Climbing;
                    g.noSpotterSince.reset();
                    g.spotterSince.reset();
                }
            } else {
                g.spotterSince.reset();
            }
        }

        view.state = g.state;
        view.enteredAlarm = g.alarmCount > alarmsBefore;
        views.push_back(std::move(view));
    }

    // 本帧没匹配上的老状态机：连续丢失超 lostSec 才销毁，
    // 短暂遮挡一两帧不丢掉已累计的 grace 计时。
    for (size_t t = 0; t < guards_.size();) {
        if (matched[t]) {
            ++t;
            continue;
        }
        Guard &g = guards_[t];
        if (!g.lostSince)
            g.lostSince = now;
        else if (now - *g.lostSince >= params_.lostSec) {
            guards_.erase(guards_.begin() + (long)t);
            matched.erase(matched.begin() + (long)t);
            continue;
        }
        ++t;
    }
    return views;
}

}  // namespace ladderguard