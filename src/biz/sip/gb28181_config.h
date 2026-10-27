#ifndef IDGE_GB28181_CONFIG_H
#define IDGE_GB28181_CONFIG_H

/**
 * @file gb28181_config.h
 * @brief GB28181 国标推流的配置结构体与 JSON 载入
 *
 * 纯数据 + 一个静态 load()，不含任何网络/编解码行为。字段默认值即国标常见值，
 * 具体校验规则和"哪些键现在还是假开关"见 gb28181_config.cpp 与同目录 README.md。
 */

#include <QString>
#include <QVector>

/// SIP 平台侧：设备向谁注册、用什么地址注册（一个 manager 一份）
struct Gb28181PlatformConfig {
    QString localHost;          ///< 本机 UDP 绑定地址，允许 0.0.0.0；必须是 IPv4 字面量
    int localSipPort = 5060;    ///< 本机 SIP 端口
    QString advertiseHost;      ///< 写进 Via/Contact 的对外地址；禁止 0.0.0.0，localHost 为通配时必须显式填
    int advertiseSipPort = 5060;
    QString serverHost;         ///< 平台 SIP 服务器地址；只认 IPv4 字面量，填域名会静默不发注册/心跳
    int serverPort = 5060;
    QString serverCode;         ///< 平台 20 位国标编码
    QString serverDomain;       ///< 平台域，缺省时用 serverHost
    QString password;           ///< 注册 Digest 口令，可被 stream 级 password 覆盖
    QString transport = QStringLiteral("udp");  ///< 仅告警用：SIP 恒走 UDP，填 tcp 不会生效
    int registerExpires = 3600; ///< 注册有效期（秒），实际提前 expires-15s 续注
    int heartbeatInterval = 30; ///< Keepalive 间隔（秒），注册成功后才开始发
    QString userAgent = QStringLiteral("idge/GB28181");
};

/// 单路视频流 = 一个国标设备通道（deviceCode 唯一，平台按它点流）
struct Gb28181StreamConfig {
    QString type;         ///< camera | rtsp | rtmp | mp4
    QString source;       ///< 取流地址；camera 可留空改用 cameraIndex
    QString deviceCode;   ///< 通道国标编码，空则整条流被 load() 丢弃
    QString channelCode;  ///< 缺省取 deviceCode
    QString serverCode;   ///< 缺省取 platform.serverCode
    QString deviceName;
    QString serialNumber;
    QString password;     ///< 缺省取 platform.password
    QString ssrc;         ///< 备用 SSRC，INVITE 带 y= 时以对方为准
    QString transport;    ///< 媒体侧 UDP/TCP，实际由 INVITE 的 SDP 决定
    int cameraIndex = -1;  ///< type=camera 且 source 为空时拼 /dev/video{index}，-1 视作 0
    // 下面四项只在"需要重编码"的路径生效：源已是 H.264 时直接 mp4toannexb 透传，
    // 宽高码率帧率一概不生效（见 gb28181_media.cpp 的 passthrough 分支）。
    int width = 1920;
    int height = 1080;
    int fps = 25;         ///< type=camera 时会被探测到的真实帧率覆盖；其它源原样使用
    int bitrate = 2000000;
    bool enabled = true;  ///< false 的流仍会被解析，但不注册不推流
    bool loop = true;     ///< 只对 type=mp4 有意义，其它源忽略
};

struct Gb28181Config {
    Gb28181PlatformConfig platform;
    QVector<Gb28181StreamConfig> streams;
    QString configPath;

    /**
     * @brief 从 config.json 读国标配置
     * @return 平台与流都齐备时 true；否则 false 且（给了 error 时）写入原因
     *
     * 认两种布局：新式 `gb28181: { platform, streams }`，以及兼容的顶层
     * `platform` + `streams`。判据不是"有没有 gb28181 键"，而是
     * serverHost 非空且至少有一条 deviceCode 完整的流，否则返回 false
     * ——调用方（main.cpp）据此决定要不要启动 manager。
     */
    static bool load(const QString &path, Gb28181Config *out, QString *error = nullptr);
};

#endif
