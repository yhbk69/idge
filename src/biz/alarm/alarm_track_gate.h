#ifndef ALARM_TRACK_GATE_H
#define ALARM_TRACK_GATE_H

// ============================================================================
// alarm_track_gate.h - 报警类别的跨帧 track 限流闸门
// ============================================================================
// 把「帧级重复检出」压成「每个目标事件一条报警」：同一目标在连续推理批次里
// 被反复检出时只确认一次事件，确认后该轨迹存续期内不再重复出报警；目标离开
// keepNs 后轨迹销毁，再进来算新事件、可再报。
//
// 语义移植自 cigarette 跟踪 demo 的 SmokeEventRecorder（min_hits / recorded /
// 全局 cooldown 兜底），但判据做了两处本地化：
//   1. 关联不跑 ByteTrack，用轻量 IoU 一对一贪心匹配——与仓内既有两处同型实现
//      同一口径（围栏 fenceTracks_、绘图 BoxSmoother），报警类别每槽位只有
//      alarm_class 一种、目标数少，够用且误配代价对称；
//   2. 确认阈值 = 命中批次数 AND 存活时长双条件。推理每 3 帧一次、各源帧率
//      不同（CSI ~7.5fps、RTSP 各异），纯帧数口径（demo 的 min_hits=10）在
//      这里等于跟着帧率漂；纯时长口径又挡不住单帧毛刺凑时长，故两者都要。
//
// allowRecord 是把 AlarmManager 既有的 2 秒全局窗口结果传进来：窗口不开时
// 轨迹状态照常推进但**不置 recorded**，下一批窗口开了再补记（retry 语义，
// 对齐 demo 的 cooldown 拦截不消耗事件）。这样闸门自己无时钟、无配置依赖，
// 可脱离报警管理器单测（tests/test_alarm_track.cpp，仿 test_ladder_guard）。
//
// 线程纪律：Gate 不是线程安全的。每通道解码线程各调各的实例（由
// AlarmManager 按 key=通道*1e6+槽位*1e3+cls_id 分流，管理器侧持 mutex_ 调用）。
// ============================================================================

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace alarmtrack {

struct Box {
    float x, y, w, h;   // 原始帧像素坐标（od 的 box 口径，跨批次一致即可算 IoU）
};

struct Params {
    int minHits = 3;                       // 最少命中推理批次数
    long long minAliveNs = 1000000000LL;   // 最短存活时长（1 秒）
    long long keepNs = 3000000000LL;       // 连续消失多久判事件结束、轨迹销毁（3 秒）
    float iouThr = 0.3f;                   // 匹配阈值，与围栏 kFenceTrackIouThr 同口径
};

class Gate {
public:
    // 每个推理批次的每个报警类别检出框调用一次。
    //   key: 目标身份分区（通道/槽位/类别号组合，见文件头）
    //   nowNs: steady_clock 单调纳秒，只与自身做差，不受墙钟跳变影响
    //   allowRecord: 调用方兜底窗口此刻是否放行；false 时确认沿保持未消耗、下批重试
    // 返回 true = 本框刚把一条候选轨迹确认为事件，调用方应产出一条报警。
    bool observe(int key, const Box &box, long long nowNs,
                 const Params &params, bool allowRecord);

    // 清空全部轨迹（限流窗口重置时同步调用，防老轨迹吃掉新事件）。
    void reset();

    // 测试内省：某 key 当前存续的轨迹数（含未确认候选）。
    int trackCount(int key) const;

private:
    struct Track {
        Box box;
        long long firstHitNs = 0;
        long long lastHitNs = 0;
        int hits = 0;
        bool recorded = false;
    };

    // 淘汰超过 keepNs 没被命中的轨迹；批次可能不带任何该 key 的检出，
    // 所以 observe 入口无条件先清一次。
    static void prune(std::vector<Track> &tracks, long long nowNs, long long keepNs);

    std::unordered_map<int, std::vector<Track>> tracks_;
};

} // namespace alarmtrack

#endif // ALARM_TRACK_GATE_H
