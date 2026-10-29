/**
 * ws_sink.cpp — 报警上行的本机回环接收端（人工对拍工具，不是自检）
 *
 * 用途：板子出网被限死，所以联调只能在 loopback 上跑。它同时认两种媒体交付模式：
 *   transfer="binary_chunks" —— 按 media_id 把二进制块拼成文件落到 /tmp/sink_out/，
 *     边收边算 sha256 与 media_begin 声明的值对比，media_end 核对块数；
 *   transfer="http" —— 不开文件不建流，只把 JSON 里那个地址原样打出来，
 *     由外部 curl 取回再与盘上文件对拍（地址模式的本轮判据）。
 * 对端（Pad）将来该做什么，这个小程序就是它的最低标准对照物。
 *
 * 运行（无需 DISPLAY，QCoreApplication）：
 *   ./build/build_rk3588_linux/ws_sink 8765
 * 然后让 idge 的 alarm_uplink.server_url 指到 ws://127.0.0.1:8765。
 *
 * 只认协议里的四种上行类型，其余类型原样打印一行，便于看信封形状。
 */

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QWebSocket>
#include <QWebSocketServer>

#include <cstdio>
#include <memory>

namespace {

/// 一条正在接收的媒体流：块按到达顺序追加，哈希边收边算（1.2MB 不必二次读盘）
/// QFile 与 QCryptographicHash 都不可拷贝，所以 Sink 里用 unique_ptr 持有，
/// "有没有在收"就是指针空不空
struct Stream {
    QFile file;
    QCryptographicHash hash{QCryptographicHash::Sha256};
    QString mediaId;
    QString kind;
    QString name;
    QString expectedSha;
    qint64 totalBytes = 0;
    int totalChunks = 0;
    int chunksReceived = 0;
};

void print(const QString &line)
{
    fprintf(stdout, "%s\n", qPrintable(line));
    fflush(stdout);
}

}  // namespace

/// 每个连接一套独立状态：媒体流是按连接串行的，不能跨连接复用
class Sink : public QObject
{
    Q_OBJECT

public:
    Sink(QWebSocket *socket, const QString &outDir, QObject *parent = nullptr)
        : QObject(parent), socket_(socket), outDir_(outDir)
    {
        connect(socket_, &QWebSocket::textMessageReceived, this, &Sink::onText);
        connect(socket_, &QWebSocket::binaryMessageReceived, this, &Sink::onBinary);
        connect(socket_, &QWebSocket::disconnected, this, &Sink::deleteLater);
        print(QStringLiteral("[sink] 客户端已连接 %1").arg(socket_->peerAddress().toString()));
    }

private slots:
    void onText(const QString &message)
    {
        const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
        if (!doc.isObject()) {
            print(QStringLiteral("[sink] 非 JSON 文本帧，长度=%1").arg(message.size()));
            return;
        }
        const QJsonObject env = doc.object();
        const QString type = env.value(QStringLiteral("type")).toString();
        const QJsonObject data = env.value(QStringLiteral("data")).toObject();

        if (type == QLatin1String("media_begin")) {
            beginStream(data);
            return;
        }
        if (type == QLatin1String("media_end")) {
            endStream(data);
            return;
        }

        // alarm_info 与其余类型：只打一行摘要，字节与长字段不进日志
        if (type == QLatin1String("alarm_info")) {
            const QJsonObject snap = data.value(QStringLiteral("snapshot")).toObject();
            print(QStringLiteral(
                      "[sink] alarm_info alarm=%1 ch=%2 type=%3 snapshot=%4 clip=%5 "
                      "transfer=%6 snapshotUrl=%7")
                      .arg(data.value(QStringLiteral("alarm_id")).toString())
                      .arg(data.value(QStringLiteral("channel")).toInt())
                      .arg(data.value(QStringLiteral("alarm_type")).toString())
                      .arg(snap.isEmpty() ? QStringLiteral("-") : QStringLiteral("y"))
                      .arg(data.contains(QStringLiteral("clip")) ? QStringLiteral("y")
                                                                 : QStringLiteral("-"))
                      .arg(snap.value(QStringLiteral("transfer")).toString())
                      .arg(snap.value(QStringLiteral("url")).toString()));
            return;
        }
        if (type == QLatin1String("media_failed")) {
            print(QStringLiteral("[sink] media_failed media=%1 kind=%2 alarm=%3 reason=%4")
                      .arg(data.value(QStringLiteral("media_id")).toString())
                      .arg(data.value(QStringLiteral("kind")).toString())
                      .arg(data.value(QStringLiteral("alarm_id")).toString())
                      .arg(data.value(QStringLiteral("reason")).toString()));
            return;
        }
        print(QStringLiteral("[sink] 文本帧 type=%1 message_id=%2")
                  .arg(type, env.value(QStringLiteral("message_id")).toString()));
    }

    void onBinary(const QByteArray &bytes)
    {
        if (!stream_) {
            print(QStringLiteral("[sink] 收到无主的二进制帧 %1 字节（没有 media_begin）")
                      .arg(bytes.size()));
            return;
        }
        stream_->hash.addData(bytes);
        stream_->file.write(bytes);
        ++stream_->chunksReceived;
    }

private:
    void beginStream(const QJsonObject &data)
    {
        const QString transfer = data.value(QStringLiteral("transfer")).toString();
        if (transfer == QLatin1String("http")) {
            // 地址模式：没有字节要收，一个字符都不落盘，把 curl 需要的字段全打出来
            print(QStringLiteral("[sink] media_begin(http) %1 kind=%2 name=%3 mime=%4 "
                                 "bytes=%5 sha256=%6 url=%7")
                      .arg(data.value(QStringLiteral("media_id")).toString())
                      .arg(data.value(QStringLiteral("kind")).toString())
                      .arg(data.value(QStringLiteral("name")).toString())
                      .arg(data.value(QStringLiteral("mime")).toString())
                      .arg(static_cast<qint64>(
                               data.value(QStringLiteral("total_bytes")).toDouble()))
                      .arg(data.value(QStringLiteral("sha256")).toString())
                      .arg(data.value(QStringLiteral("url")).toString()));
            return;
        }

        if (stream_) {
            print(QStringLiteral("[sink] 警告：media=%1 还没结束就来了新的 media=%2，旧的按不完整收尾")
                      .arg(stream_->mediaId, data.value(QStringLiteral("media_id")).toString()));
            closeAndReport(false);
        }
        stream_.reset(new Stream);
        stream_->mediaId = data.value(QStringLiteral("media_id")).toString();
        stream_->kind = data.value(QStringLiteral("kind")).toString();
        stream_->name = data.value(QStringLiteral("name")).toString();
        stream_->expectedSha = data.value(QStringLiteral("sha256")).toString();
        stream_->totalBytes = static_cast<qint64>(data.value(QStringLiteral("total_bytes")).toDouble());
        stream_->totalChunks = data.value(QStringLiteral("total_chunks")).toInt();

        const QString suffix = QFileInfo(stream_->name).suffix();
        const QString path = outDir_ + QLatin1Char('/') + stream_->kind + QLatin1Char('_')
                             + stream_->mediaId
                             + (suffix.isEmpty() ? QString() : QLatin1Char('.') + suffix);
        stream_->file.setFileName(path);
        if (!stream_->file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            print(QStringLiteral("[sink] 打开落盘文件失败 %1").arg(path));
            stream_.reset();
            return;
        }
        print(QStringLiteral("[sink] media_begin %1 kind=%2 声明=%3 字节/%4 块 → %5")
                  .arg(stream_->mediaId, stream_->kind)
                  .arg(stream_->totalBytes)
                  .arg(stream_->totalChunks)
                  .arg(path));
    }

    void endStream(const QJsonObject &data)
    {
        const QString mediaId = data.value(QStringLiteral("media_id")).toString();
        if (!stream_ || mediaId != stream_->mediaId) {
            print(QStringLiteral("[sink] media_end 对不上当前流：声明=%1 在收=%2")
                      .arg(mediaId, stream_ ? stream_->mediaId : QStringLiteral("-")));
            return;
        }
        const int seqsSent = data.value(QStringLiteral("seqs_sent")).toInt();
        closeAndReport(seqsSent == stream_->totalChunks && seqsSent == stream_->chunksReceived);
    }

    void closeAndReport(bool sentOk)
    {
        if (!stream_) {
            return;
        }
        Stream &s = *stream_;
        s.file.flush();
        s.file.close();
        const QString actual = QString::fromLatin1(s.hash.result().toHex());
        const qint64 received = s.file.size();
        print(QStringLiteral("[sink] media_end %1 收=%2/%3 字节 块=%4/%5 sha256=%6 完整=%7")
                  .arg(s.mediaId)
                  .arg(received)
                  .arg(s.totalBytes)
                  .arg(s.chunksReceived)
                  .arg(s.totalChunks)
                  .arg(actual == s.expectedSha ? QStringLiteral("match") : QStringLiteral("MISMATCH"))
                  .arg(sentOk && received == s.totalBytes && actual == s.expectedSha
                            ? QStringLiteral("yes") : QStringLiteral("no")));
        stream_.reset();
    }

    QWebSocket *socket_;
    QString outDir_;
    std::unique_ptr<Stream> stream_;
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);

    QCommandLineParser parser;
    parser.addPositionalArgument(QStringLiteral("port"), QStringLiteral("监听端口，默认 8765"));
    parser.addPositionalArgument(QStringLiteral("outdir"),
                                 QStringLiteral("媒体落盘目录，默认 /tmp/sink_out"));
    parser.process(app);

    const int port = parser.positionalArguments().value(0).toInt() > 0
        ? parser.positionalArguments().value(0).toInt() : 8765;
    const QString outDir = parser.positionalArguments().size() > 1
        ? parser.positionalArguments().at(1) : QStringLiteral("/tmp/sink_out");
    QDir().mkpath(outDir);

    QWebSocketServer server(QStringLiteral("idge-alarm-sink"),
                            QWebSocketServer::NonSecureMode);
    if (!server.listen(QHostAddress::LocalHost, port)) {
        print(QStringLiteral("[sink] 监听 %1 失败: %2").arg(port).arg(server.errorString()));
        return 1;
    }
    print(QStringLiteral("[sink] 已监听 ws://127.0.0.1:%1，媒体落 %2").arg(port).arg(outDir));

    QObject::connect(&server, &QWebSocketServer::newConnection, &server,
                     [&server, outDir]() {
                         new Sink(server.nextPendingConnection(), outDir);
                     });

    return app.exec();
}

#include "ws_sink.moc"
