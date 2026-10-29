#ifndef UPLINK_TRANSPORT_H
#define UPLINK_TRANSPORT_H

/**
 * @file uplink_transport.h
 * @brief 报警上行的传输接缝（纯抽象，无实现）
 *
 * 为什么要有这一层：`AlarmUplinkEngine` 只关心"控制面 JSON + 媒体二进制字节按序送出去"，
 * 不关心底下是 WebSocket 文本/二进制帧、HTTP PUT，还是别的通道。本轮只实现 WS 一种
 * （`WsClient` 直接继承本接口），将来换传输只动实现方，引擎里的合流、队列、
 * 协议语义全部原样复用。
 *
 * 也让离线自检成为可能：`tests/test_alarm_uplink.cpp` 用一个 FakeTransport 记录
 * 发出去的每一帧，不需要把 WsClient（以及它牵出的 Qt WebSockets / GUI 线程单例）
 * 拖进测试目标。
 *
 * 线程约定：实现方与调用方在同一个线程（当前是 GUI 线程）。本接口不加锁。
 */

#include <QString>
#include <QByteArray>

namespace uplink {

class UplinkTransport
{
public:
    virtual ~UplinkTransport() = default;

    /// 链路是否可用。false 时 sendText/sendBinary 不会被调用（调用方先排队）
    virtual bool transportConnected() const = 0;

    /**
     * @brief 发送水位闸门：实现方写缓冲还没堆到阈值时返回 true
     *
     * 媒体字节是 MB 级的，不加闸门就等于让它们进 Qt 的无界写缓冲——那会把 GUI 线程
     * 的绘制挤出去（本项目 4 路 1080p 的零卡顿是明确成果）。调用方据此**逐块**投喂，
     * 发不动就等下一次定时器，绝不批量灌。
     */
    virtual bool readyForMore() const = 0;

    /// 发一个文本帧（控制面）。返回实际写入字节数，<=0 表示失败/未连接
    virtual qint64 sendText(const QString &text) = 0;

    /// 发一个二进制帧（媒体面，一次调用恰好一个不拆分的数据帧）。同上返回写入字节数
    virtual qint64 sendBinary(const QByteArray &bytes) = 0;
};

}  // namespace uplink

#endif // UPLINK_TRANSPORT_H
