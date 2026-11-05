// ============================================================================
// test_pad_http_message.cpp — v1 HTTP 面的报文解析离线自检（不起 socket）
// ----------------------------------------------------------------------------
// 直接喂字节给 padhttp::RequestDecoder，验"传输层形状"这一件事。
// 为什么值得单独钉：这一层的每个判据都对应一种现场故障——
//   · chunked 解错 = Pad 的 multipart 处置提交收到 400，而报文看起来完全正常；
//   · 上限在收完之后才判 = 一句 `Content-Length: 2G` 就能把整机内存吃掉；
//   · multipart 用 filename 拼路径 = 路径穿越（本层不参与路径，但**解析出的
//     filename 会被上层看见**，所以这里也要验它只是数据、不是判据）；
//   · 百分号解码把非法转义丢掉 = 一个畸形 ID 从"对象不存在"变成"参数错误"，方向就错了。
//
// 覆盖十九条契约：
//   1) 简单 GET：方法/路径/查询/版本/头都解出来，且没有正文时直接 Complete
//   2) 请求行不是三段 → Failed（不崩、不半解）
//   3) 路径与查询的百分号解码（中文 taskId 这种现场写法）
//   4) 非法转义 `%zz` 原样保留，不吞字节、不报错
//   5) 同名查询键保留**第一个**
//   6) Content-Length 正文分两次喂（半截头 + 半截正文）也能拼完整
//   7) 头超过 maxHeadBytes → Failed
//   8) Content-Length 声明就超上限 → Failed，且**不先收正文**（receivedBodyBytes 仍是 0）
//   9) 正文实际字节超上限 → Failed
//  10) chunked：两段数据 + 尾 chunk + trailer → 正文逐字节还原
//  11) chunked 长度行不是十六进制 → Failed
//  12) keep-alive：takeRequest 后同一条连接接着解第二个请求，第一个的字节不残留
//  13) multipart：payload(JSON) + 2 张 photos 的 name/filename/contentType/data 全对
//  14) multipart 缺结束定界 → parseMultipart false
//  15) multipart part 数超上限 → false（不是"截断前 N 个"）
//  16) 信封成功形状：code/message/detail=null/data 有内容
//  17) 信封失败形状：detail 非空进 detail，data 恒 null
//  18) serializeReply：Content-Length 与正文一致、keep-alive/close、extra 头能顶掉默认头
//  19) 400/2001 与 422/4001 两条便捷出口的 code/状态码配对（§2.4 那张表）
//
// 仓库根运行：./build/build_rk3588_linux/test_pad_http_message，rc 0=PASS 1=FAIL
// ============================================================================

#include "pad_http_message.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

#include <iostream>

namespace {

int failures = 0;

void check(bool ok, const char *message)
{
    if (!ok) {
        ++failures;
        std::cout << "  FAIL: " << message << std::endl;
    }
}

/// 造一个只喂字节、不碰 socket 的解码器
bool feedAll(const QByteArray &raw, padhttp::Request *out, QString *error,
             int maxHead = 16384, qint64 maxBody = 26LL * 1024 * 1024)
{
    padhttp::RequestDecoder decoder;
    decoder.setLimits(maxHead, maxBody);
    if (!decoder.feed(raw)) {
        if (error)
            *error = decoder.errorText();
        return false;
    }
    if (!decoder.isComplete()) {
        if (error)
            *error = QStringLiteral("没收完（还在等字节）");
        return false;
    }
    if (!decoder.takeRequest(out)) {
        if (error)
            *error = decoder.errorText();
        return false;
    }
    return true;
}

QByteArray simpleGet(const QString &target)
{
    return "GET " + target.toUtf8() + " HTTP/1.1\r\n"
           "Host: 127.0.0.1\r\n"
           "\r\n";
}

}  // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    std::cout << "=== test_pad_http_message ===" << std::endl;

    // ── 1) 简单 GET ────────────────────────────────────────────────────
    {
        padhttp::Request request;
        QString error;
        const bool ok = feedAll(simpleGet("/api/health?taskId=T-1"), &request, &error);
        check(ok, qPrintable(QStringLiteral("契约1：简单 GET 要解完成（%1）").arg(error)));
        if (ok) {
            check(request.method == QLatin1String("GET"), "契约1：方法大写");
            check(request.path == QStringLiteral("/api/health"), "契约1：路径不含 query");
            check(request.version == QStringLiteral("HTTP/1.1"), "契约1：版本原样留着");
            check(request.queryValue(QStringLiteral("taskId")) == QStringLiteral("T-1"),
                  "契约1：查询参数解出来");
            check(request.header(QStringLiteral("HOST")).contains(QStringLiteral("127.0.0.1")),
                  "契约1：头名大小写不敏感");
            check(request.body.isEmpty(), "契约1：GET 没有正文");
        }
    }

    // ── 2) 请求行不是三段 ──────────────────────────────────────────────
    {
        padhttp::Request request;
        QString error;
        check(!feedAll("GARBAGE\r\n\r\n", &request, &error), "契约2：垃圾请求行要判失败");
        check(!error.isEmpty(), "契约2：失败要有中文原因");
    }

    // ── 3/4) 百分号解码 ────────────────────────────────────────────────
    {
        const QString decoded = padhttp::percentDecode(
            QStringLiteral("/api/fences/BJ5-%E4%B8%AD%E5%9B%BD"));
        check(decoded.contains(QStringLiteral("中国")), "契约3：UTF-8 转义要还原成中文");
        const QString illegal = padhttp::percentDecode(QStringLiteral("a%zzb"));
        check(illegal == QStringLiteral("a%zzb"), "契约4：非法转义原样保留（不吞字节）");
        check(padhttp::percentDecode(QStringLiteral("100%")) == QStringLiteral("100%"),
              "契约4：结尾孤立的 % 也要保留");
    }

    // ── 5) 同名查询键保留第一个 ────────────────────────────────────────
    {
        const QMap<QString, QString> query =
            padhttp::parseQuery(QStringLiteral("taskId=A&taskId=B&x=%E4%B8%AD"));
        check(query.value(QStringLiteral("taskId")) == QStringLiteral("A"),
              "契约5：同名键取第一个（不是最后一个）");
        check(query.value(QStringLiteral("x")) == QStringLiteral("中"),
              "契约5：值一律解码");
    }

    // ── 6) 分两次喂（半截头 + 半截正文）────────────────────────────────
    {
        const QByteArray raw = "POST /api/x HTTP/1.1\r\nContent-Length: 5\r\n"
                               "Content-Type: application/json\r\n\r\n12345";
        padhttp::RequestDecoder decoder;
        decoder.setLimits(16384, 1024);
        check(decoder.feed(raw.left(20)), "契约6：前半段喂进去不报错");
        check(decoder.state() == padhttp::RequestDecoder::State::ReadingHead,
              "契约6：头没收完就是 ReadingHead");
        check(decoder.feed(raw.mid(20)), "契约6：后半段喂进去不报错");
        check(decoder.isComplete(), "契约6：补齐后 Complete");
        padhttp::Request request;
        check(decoder.takeRequest(&request) && request.body == QByteArray("12345"),
              "契约6：正文按 Content-Length 收全");
    }

    // ── 7) 头超上限 ────────────────────────────────────────────────────
    {
        QByteArray raw = "GET /api/health HTTP/1.1\r\nX-Big: ";
        raw += QByteArray(2000, 'a');
        raw += "\r\n\r\n";
        padhttp::Request request;
        QString error;
        check(!feedAll(raw, &request, &error, 1024, 1024), "契约7：头超上限要当场失败");
        check(error.contains(QStringLiteral("超过")), "契约7：原因里要说清是超限");
    }

    // ── 8) 声明就超上限，且不先收正文 ──────────────────────────────────
    {
        QByteArray raw = "POST /api/x HTTP/1.1\r\nContent-Length: 99999\r\n\r\n";
        raw += QByteArray(1024, 'z');   // 故意再多喂一点：本不该被收下
        padhttp::RequestDecoder decoder;
        decoder.setLimits(16384, 4096);
        check(!decoder.feed(raw), "契约8：Content-Length 超上限直接失败");
        check(decoder.receivedBodyBytes() == 0,
              "契约8：失败时正文一个字节都不该已经收下（这是内存防线）");
    }

    // ── 9) 正文实际字节超上限 ──────────────────────────────────────────
    {
        QByteArray raw = "POST /api/x HTTP/1.1\r\nContent-Length: 8000\r\n\r\n";
        raw += QByteArray(8000, 'z');
        padhttp::Request request;
        QString error;
        check(!feedAll(raw, &request, &error, 16384, 4096), "契约9：正文超上限失败");
    }

    // ── 10) chunked 还原 ───────────────────────────────────────────────
    {
        QByteArray raw = "POST /api/x HTTP/1.1\r\n"
                         "Transfer-Encoding: chunked\r\n"
                         "\r\n"
                         "5\r\nhello\r\n"
                         "6;ext=1\r\n world\r\n"
                         "0\r\n"
                         "X-Ignore: yes\r\n"
                         "\r\n";
        padhttp::Request request;
        QString error;
        const bool ok = feedAll(raw, &request, &error);
        check(ok, qPrintable(QStringLiteral("契约10：chunked 要能解完（%1）").arg(error)));
        if (ok)
            check(request.body == QByteArray("hello world"),
                  "契约10：chunk 拼接后的正文逐字节正确");
    }

    // ── 11) chunk 长度行非法 ───────────────────────────────────────────
    {
        QByteArray raw = "POST /api/x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
                         "zz\r\nhello\r\n";
        padhttp::Request request;
        QString error;
        check(!feedAll(raw, &request, &error), "契约11：非十六进制的长度行要失败");
    }

    // ── 12) keep-alive：同一条连接上连解两个请求 ───────────────────────
    {
        padhttp::RequestDecoder decoder;
        decoder.setLimits(16384, 4096);
        const QByteArray two = simpleGet("/api/one") + simpleGet("/api/two");
        check(decoder.feed(two), "契约12：两个请求一起喂不报错");
        padhttp::Request first;
        padhttp::Request second;
        check(decoder.takeRequest(&first) && first.path == QStringLiteral("/api/one"),
              "契约12：先解出第一个");
        check(decoder.isComplete(), "契约12：第二个的字节留在缓冲里，不用等新的 readyRead");
        check(decoder.takeRequest(&second) && second.path == QStringLiteral("/api/two"),
              "契约12：接着解出第二个");
        check(second.body.isEmpty() && second.query.isEmpty(),
              "契约12：takeRequest 必须把正文与查询复位（残留就是串请求）");
    }

    // ── 13) multipart：payload + 两张照片 ─────────────────────────────
    {
        const QByteArray boundary = "----QtFormBoundary7MA4YWxkTrZu0gW";
        QByteArray body;
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"payload\"\r\n";
        body += "Content-Type: application/json\r\n\r\n";
        body += "{\"requestId\":\"r-1\",\"measure\":\"已现场纠正\"}\r\n";
        body += "--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"photos\"; filename=\"../../etc/passwd\"\r\n";
        body += "Content-Type: image/jpeg\r\n\r\n";
        body += QByteArray::fromHex("ffd8ffe0");   // 三个 part 的字节都要原样
        body += "\r\n--" + boundary + "\r\n";
        body += "Content-Disposition: form-data; name=\"photos\"; filename=\"b.png\"\r\n";
        body += "Content-Type: image/png\r\n\r\n";
        body += QByteArray::fromHex("89504e47");
        body += "\r\n--" + boundary + "--\r\n";

        const QByteArray raw = "POST /api/alarms/A1/dispose HTTP/1.1\r\n"
                              "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
                              "Content-Length: " + QByteArray::number(body.size()) + "\r\n\r\n"
                              + body;
        padhttp::Request request;
        QString error;
        const bool ok = feedAll(raw, &request, &error);
        check(ok, qPrintable(QStringLiteral("契约13：multipart 请求要解完成（%1）").arg(error)));
        if (ok) {
            check(request.boundary == QString::fromLatin1(boundary), "契约13：boundary 取出来了");
            check(request.parts.size() == 3, "契约13：三个 part（payload + 两张照片）");
            if (request.parts.size() == 3) {
                check(request.parts.at(0).name == QStringLiteral("payload")
                        && request.parts.at(0).data.contains("measure"),
                      "契约13：payload part 的正文是那份 JSON");
                check(request.parts.at(1).name == QStringLiteral("photos")
                        && request.parts.at(1).data == QByteArray::fromHex("ffd8ffe0"),
                      "契约13：照片 part 的字节逐字节原样（不能被当文本转码）");
                check(request.parts.at(1).filename == QStringLiteral("../../etc/passwd"),
                      "契约13：filename 只是**数据**——本层不清洗它，清洗/不用它归上层");
                check(request.parts.at(2).contentType == QStringLiteral("image/png"),
                      "契约13：per-part 的 Content-Type 解出来了");
            }
        }
    }

    // ── 14) 缺结束定界 ─────────────────────────────────────────────────
    {
        QList<padhttp::Part> parts;
        QString error;
        const QByteArray body = "--B\r\nContent-Disposition: form-data; name=\"a\"\r\n\r\nx\r\n";
        check(!padhttp::parseMultipart(body, QStringLiteral("B"), &parts, &error),
              "契约14：没有结束定界串要失败");
        check(error.contains(QStringLiteral("定界")), "契约14：原因要说清是定界问题");
    }

    // ── 15) part 数超上限 ──────────────────────────────────────────────
    {
        QByteArray body;
        for (int i = 0; i < 4; ++i) {
            body += "--B\r\nContent-Disposition: form-data; name=\"p";
            body += QByteArray::number(i);
            body += "\"\r\n\r\nx\r\n";
        }
        body += "--B--\r\n";
        QList<padhttp::Part> parts;
        QString error;
        check(!padhttp::parseMultipart(body, QStringLiteral("B"), &parts, &error, 2),
              "契约15：part 超上限要失败，而不是悄悄只留前两个");
        check(parts.size() <= 2, "契约15：失败时 out 里不超过上限（不做半份交付）");
    }

    // ── 16/17) 信封 ────────────────────────────────────────────────────
    {
        QJsonObject data;
        data.insert(QStringLiteral("ok"), true);
        const QJsonDocument okDoc =
            QJsonDocument::fromJson(padhttp::envelopeJson(0, QStringLiteral("ok"), QString(), data));
        check(okDoc.object().value(QStringLiteral("code")).toInt() == 0, "契约16：code=0");
        check(okDoc.object().value(QStringLiteral("message")).toString() == QStringLiteral("ok"),
              "契约16：message 可直接展示");
        check(okDoc.object().value(QStringLiteral("detail")).isNull(),
              "契约16：detail 空串要出 null（不是空字符串）");
        check(okDoc.object().value(QStringLiteral("data")).isObject(), "契约16：data 有内容");

        const QJsonDocument errDoc = QJsonDocument::fromJson(padhttp::envelopeJson(
            2001, QStringLiteral("参数错误"), QStringLiteral("alarmId=坏值"),
            QJsonValue(QJsonValue::Null)));
        check(errDoc.object().value(QStringLiteral("data")).isNull(),
              "契约17：错误响应的 data 恒 null");
        check(errDoc.object().value(QStringLiteral("detail")).toString()
                  .contains(QStringLiteral("alarmId")),
              "契约17：detail 只进日志、给现场排障用");
    }

    // ── 18) serializeReply ─────────────────────────────────────────────
    {
        padhttp::Reply reply = padhttp::replyOk(QJsonObject{});
        reply.extraHeaders.append(qMakePair(QStringLiteral("X-Trace"), QStringLiteral("abc")));
        const QByteArray wire = padhttp::serializeReply(reply, true);
        check(wire.startsWith("HTTP/1.1 200 OK\r\n"), "契约18：状态行");
        const int headEnd = wire.indexOf("\r\n\r\n");
        const QByteArray head = wire.left(headEnd);
        const QByteArray body = wire.mid(headEnd + 4);
        check(head.contains("Content-Length: " + QByteArray::number(body.size())),
              "契约18：Content-Length 必须与正文实际字节数一致");
        check(head.contains("Connection: keep-alive"), "契约18：keep-alive");
        check(head.contains("X-Trace: abc"), "契约18：追加头要写出去");
        const QByteArray again = padhttp::serializeReply(reply, false);
        check(again.contains("Connection: close"), "契约18：close 档");
        // extra 里自己给了 Content-Type 时不能再写第二个（两个同名头=客户端用最后一个）
        padhttp::Reply custom;
        custom.extraHeaders.append(
            qMakePair(QStringLiteral("Content-Type"), QStringLiteral("text/plain")));
        custom.body = "hi";
        const QByteArray customWire = padhttp::serializeReply(custom, true);
        int occurrences = customWire.toLower().count("content-type:");
        check(occurrences == 1, "契约18：Content-Type 只能出现一次（默认值要顶掉）");
    }

    // ── 19) 状态码/业务码配对 ─────────────────────────────────────────
    {
        const padhttp::Reply bad = padhttp::replyBadRequest(QStringLiteral("少字段"));
        const padhttp::Reply validation = padhttp::replyValidation(QStringLiteral("形状非法"));
        const padhttp::Reply gone = padhttp::replyNotFound(QStringLiteral("没这条"));
        const padhttp::Reply clash = padhttp::replyConflict(QStringLiteral("已经处置过"));
        const padhttp::Reply draft = padhttp::replyConflictCode(2004, QStringLiteral("有草稿"));
        check(bad.status == 400 && bad.body.contains("\"code\":2001"),
              "契约19：400 配 2001（§2.4）");
        check(validation.status == 422 && validation.body.contains("\"code\":4001"),
              "契约19：422 配 4001（§2.4）");
        check(gone.status == 404 && gone.body.contains("\"code\":2002"), "契约19：404 配 2002");
        check(clash.status == 409 && clash.body.contains("\"code\":2003"), "契约19：409 配 2003");
        check(draft.status == 409 && draft.body.contains("\"code\":2004"),
              "契约19：同一状态码可以配不同业务码（围栏那三档）");
    }

    std::cout << (failures == 0 ? "PASS" : "FAIL") << " (" << failures << " 条失败)" << std::endl;
    return failures == 0 ? 0 : 1;
}
