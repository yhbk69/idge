// ============================================================================
// alarm_track_gate.cpp - 见头文件契约与 tests/test_alarm_track.cpp 断言清单
// ============================================================================
#include "alarm_track_gate.h"

#include <algorithm>

namespace alarmtrack {

namespace {

float iouOf(const Box &a, const Box &b)
{
    const float ix1 = std::max(a.x, b.x);
    const float iy1 = std::max(a.y, b.y);
    const float ix2 = std::min(a.x + a.w, b.x + b.w);
    const float iy2 = std::min(a.y + a.h, b.y + b.h);
    const float inter = std::max(0.0f, ix2 - ix1) * std::max(0.0f, iy2 - iy1);
    return inter / (a.w * a.h + b.w * b.h - inter + 1e-6f);
}

} // namespace

bool Gate::observe(int key, const Box &box, long long nowNs,
                   const Params &params, bool allowRecord)
{
    std::vector<Track> &tracks = tracks_[key];
    prune(tracks, nowNs, params.keepNs);

    // 与本批已命中的轨迹跳过（lastHitNs==nowNs 即"本批已计过"）：
    // 同批重复框（NMS 漏网的近重叠框）不该把 hits 灌成两倍，
    // hits 的口径必须是"命中的批次数"。
    int bestIdx = -1;
    float bestIou = params.iouThr;
    for (size_t t = 0; t < tracks.size(); ++t) {
        if (tracks[t].lastHitNs == nowNs) continue;
        const float iou = iouOf(tracks[t].box, box);
        if (iou > bestIou) { bestIou = iou; bestIdx = (int)t; }
    }

    Track *track = nullptr;
    if (bestIdx >= 0) {
        track = &tracks[(size_t)bestIdx];
        track->box = box;
        track->lastHitNs = nowNs;
        track->hits++;
    } else {
        tracks.push_back(Track{box, nowNs, nowNs, 1, false});
        track = &tracks.back();
        // 新候选 hits=1，未达 minHits 直接不出确认沿；minHits<=0 的脏配置
        // 也不让"首帧即报警"绕过确认，按 1 处理
        if (params.minHits < 1) return false;
    }

    if (track->recorded) return false;
    if (track->hits < std::max(1, params.minHits)) return false;
    if (nowNs - track->firstHitNs < params.minAliveNs) return false;
    if (!allowRecord) return false;   // 兜底窗口未开：不置 recorded，下批重试

    track->recorded = true;
    return true;
}

void Gate::reset()
{
    tracks_.clear();
}

int Gate::trackCount(int key) const
{
    const auto it = tracks_.find(key);
    return it == tracks_.end() ? 0 : (int)it->second.size();
}

void Gate::prune(std::vector<Track> &tracks, long long nowNs, long long keepNs)
{
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(),
                                [nowNs, keepNs](const Track &t) {
                                    return nowNs - t.lastHitNs > keepNs;
                                }),
                 tracks.end());
}

} // namespace alarmtrack
