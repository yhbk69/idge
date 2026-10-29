#ifndef ALARM_UPLINK_H
#define ALARM_UPLINK_H

/**
 * @file alarm_uplink.h
 * @brief 报警上行编排（GUI 线程单例）：订阅本机报警 → 两段式发往 Pad
 *
 * 职责边界很窄：把 AlarmManager 的 AlarmRecord 折算成引擎要的纯数据、按配置装配引擎、
 * 决定"什么时候再泵一次"。报文形状、合流、队列都在 AlarmUplinkEngine 里，
 * 字节怎么出去在 UplinkTransport（当前实现是 WsClient）里。
 *
 * 媒体怎么交出去有两档（配置 alarm_uplink.media_transport，本类是唯一的分叉点）：
 *   "url"（默认）  —— 本机内嵌 http 文件服务，JSON 只带地址，对端自己下载；
 *                     截图地址在 alarm_info 里就给，片段地址等 .mp4 落盘后用 media_begin 给
 *   "ws_binary"    —— 字节走 WS 二进制分块帧（begin + N 块 + end）
 * 两档共用同一条队列/水位/ack 链路，差别只在第二段的内容。
 * `mediaTransport="url"` 但地址前缀是空串（服务没起来）时，引擎按"地址给不出"直接把
 * 两条媒体腿都不申报，只发报警文本；本类负责在 attach() 打一条 warning 说清为什么没图没视频。
 *
 * 线程模型：全程 GUI 线程。alarmGenerated 由解码线程发出，这里用 Qt::QueuedConnection
 * 收到 GUI 线程再处理——引擎本身不加锁，靠"只在 GUI 线程被调用"这条约束成立。
 *
 * 出厂默认 alarm_uplink.enabled=false：attach() 只记一条日志、一个信号都不接，
 * 关着的时候本模块对主程序是零负载。
 */

#include <QObject>
#include <QTimer>

#include "alarm_manager.h"          // AlarmRecord
#include "alarm_uplink_engine.h"

class AlarmUplink : public QObject
{
    Q_OBJECT

public:
    static AlarmUplink &instance();

    /**
     * @brief 按配置接通告警链路（frmMain 建完 WsClient 之后调一次）
     *
     * 未启用时不调用任何 connect，也不会起定时器。重复调用无副作用（attached_ 拦住）。
     */
    void attach();
    /// 断开订阅并清空待发队列（退出流程用；WsClient 自己会断链路）
    void detach();
    bool isAttached() const { return attached_; }

    /**
     * @brief 装配点（frmMain）告知媒体地址前缀，必须在 attach() 之前调
     *
     * 为什么由外面传而不是自己问 HttpFileService：依赖方向是 base ← media ← biz ← ui，
     * biz 反过来 include media 域的头就是破例。frmMain 起服务、拿到底址、注进来，
     * 三件事在同一处发生，读代码的人一眼能看到"地址从哪来"。
     * @param baseUrl  形如 `http://192.168.1.50:8090`（不带末尾斜杠）。
     *                 **空串 = 地址给不出来**（服务没起来 / 没有可用 IPv4 且没配前缀），
     *                 attach() 会据此把两条媒体腿整体关掉，只发 alarm_info 文本。
     */
    void setMediaBaseUrl(const QString &baseUrl);

signals:
    /// (category, message)，frmMain 接到调试页日志区，与 WsClient::logMessage 同一口径
    void logMessage(const QString &category, const QString &message);

private slots:
    void onAlarmGenerated(const AlarmRecord &alarm);    /// 片段落盘回填（ClipWriter 后台线程发）：把 alarm_info 承诺的 clip 变成真正的地址
    /// videoPath 是 Annex-B 裸流（DB 里存的那个），webPath 是同名 .mp4（url 模式发的那个）
    void onVideoClipAttached(const QStringList &alarmIds, const QString &videoPath,
                             const QString &webPath);
    /// 片段确定不会有了（队列丢最旧 / 录像器没就绪 / dump 失败）：清承诺 + 发 media_failed
    void onVideoClipSkipped(const QStringList &alarmIds, const QString &reason);
    /// 写缓冲水位退下来之后再泵一次；队列空了就自己停，不留常驻定时器
    void onFlushTick();
    /// 链路重连：正在传的那一段从第一块重发（对端拼不出半截流），然后把积压泵出去
    void onLinkConnected();
    /// 对端回的 uplink_ack（expect_ack=true 才有意义；默认关着，这条分支一直空转）
    void onUplinkAcked(const QString &messageId, const QString &mediaId, const QString &phase,
                       const QString &status, const QString &error);

private:
    explicit AlarmUplink(QObject *parent = nullptr);
    ~AlarmUplink() override;
    Q_DISABLE_COPY(AlarmUplink)

    /// 发一轮；还有剩就按需重挂 20ms 单发定时器
    void pumpAndReschedule();
    /**
     * @brief 把一段媒体交给后台读盘，读好回来才进引擎队列
     *
     * 每段媒体只该走这一次（判据是 SubmitResult 里的 first），所以调用点必须拿
     * 引擎的回执，而不是自己按路径判重——两处判重迟早对不上。
     */
    void requestMedia(const QString &mediaId, const QString &alarmId, const QString &kind,
                      const QString &path, const QString &mime);

    uplink::AlarmUplinkEngine engine_;
    QTimer *flushTimer_ = nullptr;
    bool attached_ = false;
    QString clipMime_;      ///< attach() 时按 recording.codec 定，片段落盘后当 MIME 兜底
    /// 装配点注进来的媒体地址前缀，空 = 地址给不出来（见 setMediaBaseUrl）
    QString mediaBaseUrl_;
    /// 文件服务的根目录（= alarmsDir() 的绝对化），mediaUrlFor 的换算基准
    QString mediaRootDir_;
    /// media_transport 折算结果，与引擎 Settings 里那份同源（attach() 一次定，运行期不变）。
    /// 默认 false 与引擎一致：没 attach 过就没有任何媒体路径可走
    bool mediaViaUrl_ = false;
};

#endif // ALARM_UPLINK_H
