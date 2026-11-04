#ifndef IDGE_GB28181_CONFIG_H
#define IDGE_GB28181_CONFIG_H

/**
 * @file gb28181_config.h
 * @brief GB28181 国标推流的配置结构体与 JSON 载入
 *
 * 纯数据 + 一个静态 load()，不含任何网络/编解码行为。字段默认值即国标常见值，
 * 具体校验规则和"哪些键现在还是假开关"见 gb28181_config.cpp 与同目录 README.md。
 */

#include "ConfigManager.h"

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

/**
 * @brief 单路视频流 = 一个国标设备通道（deviceCode 唯一，平台按它点流）
 *
 * 这里**只放国标身份 + 推流编码参数**。"这路从哪儿取流、什么分辨率/帧率/是否循环"
 * 全部由 `sourceId` 指向的 `video.sources[]` 决定（与 `video.channels[].source_id`
 * 引用同一张表），因为那是流的属性而不是国标通道的属性：两处各写一遍地址，
 * 现场改摄像头就要改两个地方，漏一个就是"界面有画、平台点流黑屏"。
 * 取用时走 `takeSource()`，不要在别处拼判据。
 */
struct Gb28181StreamConfig {
    QString sourceId;     ///< 引用 video.sources[] 的 id；空则整条流被 load() 丢弃
    QString deviceCode;   ///< 通道国标编码，空则整条流被 load() 丢弃
    QString channelCode;  ///< 缺省取 deviceCode
    QString serverCode;   ///< 缺省取 platform.serverCode
    QString deviceName;
    QString serialNumber;
    QString password;     ///< 缺省取 platform.password
    QString ssrc;         ///< 备用 SSRC，INVITE 带 y= 时以对方为准
    QString transport;    ///< 媒体侧 UDP/TCP，实际由 INVITE 的 SDP 决定
    // 只在"需要重编码"的路径生效：源已是 H.264 时直接 mp4toannexb 透传，
    // 码率与（注册表没给宽高时）那对默认宽高一个都不看。
    int bitrate = 2000000;
    bool enabled = true;  ///< false 的流仍会被解析，但不注册不推流

    /**
     * @brief 这路的取流定义（地址/类型/宽高帧率/输入格式/loop/硬解开关）
     * @return 命中注册表时是那条流；未命中（id 悬空或注册表被删）返回空 cfg
     *
     * 开流时现取，所以设置页改了 `video.sources[]` 之后**下一次 INVITE 就用新地址**，
     * 不必重启（`load()` 那份快照只用于"要不要启动 manager"和身份字段）。
     * 宽高/帧率为 0、inputFormat 为空 = 注册表没指定，由媒体层落回国标侧的老默认值。
     */
    VideoSourceCfg takeSource() const;
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
