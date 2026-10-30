// ============================================================================
// test_construction_qr.cpp — 施工信息解析/校验/会话 + 二维码离线解码自检
// ----------------------------------------------------------------------------
// 只链 Qt5::Core + Qt5::Gui + zbar + construction_session.cpp + qr_decoder.cpp：
// 不开摄像头、不碰 GUI 线程、不读 config.json。
//
// 为什么值得单独钉：登录门的全部正确性都压在"从码里解出的原文 → 校验 → 会话"这一条
// 短链上，而它在板端只靠肉眼验证一次就会定型。缺字段被放过 = 未登录也能出报警；
// 校验写歪一个键名 = 现场每张码都登不上，且提示会说"缺的是另一个字段"。
//
// 覆盖十条契约：
//   1) 完整 JSON：五个具名字段各归其位，raw 原样留着全部键
//   2) 缺 construction_id → 拒绝登录，且中文原因点名 construction_id
//   3) 缺 worker_name → 拒绝登录，且中文原因点名 worker_name
//   4) 值是数字（工号/id 常被写成 number）或带首尾空白 → 一样取到并去掉空白
//   5) 空串 / 非 JSON / JSON 数组 → 各自拒绝，原因都非空（不许静默返回空对象）
//   6) toJson 往返：未知键不丢，五个具名字段以结构体为准
//   7) 会话：startLogin 失败**不动**当前会话（不许出现半登录状态）
//   8) logout 后 snapshot 为空、isLoggedIn 为假；display 把空值统一成 "—"
//   9) 离线解码：把 /tmp/idge_test_qr.png 解回原文 → 同一条 startLogin 路径通过
//   10) HAS_ZBAR 没编进来时 scanQrFromImage 返回 false 且给出原因，而不是"解出 0 个码"
//
// 第 9 条需要一张码图（本文件不改写它，缺了就 SKIP 并打出重新生成的命令）：
//   python -m segno -o /tmp/idge_test_qr.png --scale 8 --border 4 \
//     '{"worker_name":"张三","worker_phone":"13800001111","worker_id":"GZ-0042",\
//      "work_content":"隧道道床修补","construction_id":"SG-2026-11"}'
// 码内容就是施工信息 JSON 本身（用户已确认"摄像头扫现场码、码内容即信息"），
// 所以这张图等价于现场张贴的码，解出来的原文与登录页拿到的是同一种东西。
//
// 仓库根运行：./build/build_rk3588_linux/test_construction_qr [码图路径 ...]，rc 0=PASS 1=FAIL
// ============================================================================

#include "construction_session.h"
#include "qr_decoder.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QFileInfo>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

using session::ConstructionInfo;
using session::ConstructionSession;

namespace {

void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

bool quoted(const QString &text, const char *needle)
{
    return text.contains(QLatin1String(needle));
}

QJsonObject obj(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        throw std::runtime_error("测试用的 JSON 字面量写错了");
    }
    return doc.object();
}

// 契约 1
void testFullPayload()
{
    const QString text = QStringLiteral(
        "{\"worker_name\":\"张三\",\"worker_phone\":\"13800001111\","
        "\"worker_id\":\"GZ-0042\",\"work_content\":\"隧道道床修补\","
        "\"construction_id\":\"SG-2026-11\"}");

    ConstructionInfo info;
    QString err;
    check(ConstructionInfo::fromQrText(text, &info, &err), "完整载荷应当被接受");
    check(err.isEmpty(), "接受时不该带原因");
    check(info.workerName == QStringLiteral("张三"), "负责人姓名取值错误");
    check(info.workerPhone == QStringLiteral("13800001111"), "电话取值错误");
    check(info.workerId == QStringLiteral("GZ-0042"), "工号取值错误");
    check(info.workContent == QStringLiteral("隧道道床修补"), "施工内容取值错误");
    check(info.constructionId == QStringLiteral("SG-2026-11"), "施工id取值错误");
    check(info.raw.keys().size() == 5, "raw 没原样留下全部键");
    check(!info.isEmpty(), "有内容的对象不应判为空");
}

// 契约 2 + 3：必填校验，且点名的是缺的那一个
void testRequiredFields()
{
    ConstructionInfo info;
    QString err;

    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral("{\"worker_name\":\"李四\"}"), &info, &err),
          "缺施工id 必须拒绝登录");
    check(quoted(err, "construction_id"), "原因必须点名 construction_id");
    check(!quoted(err, "worker_name"), "原因不该把在场的字段也说成缺失");

    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral("{\"construction_id\":\"SG-1\"}"), &info, &err),
          "缺负责人姓名必须拒绝登录");
    check(quoted(err, "worker_name"), "原因必须点名 worker_name");

    // 空串等价于缺字段（码上留了键但没填值，是最常见的一种贴错）
    err.clear();
    check(!ConstructionInfo::fromQrText(
              QStringLiteral("{\"construction_id\":\"SG-1\",\"worker_name\":\"  \"}"),
              &info, &err),
          "只有空白的负责人姓名必须拒绝登录");
}

// 契约 4：数字值与首尾空白
void testScalarCoercion()
{
    ConstructionInfo info;
    QString err;
    const QString text = QStringLiteral(
        "{\"construction_id\": 20260112, \"worker_name\":\"  王五  \",\"worker_id\": 42}");
    check(ConstructionInfo::fromQrText(text, &info, &err), "数字值不应被拒");
    check(info.constructionId == QStringLiteral("20260112"), "整数施工id 应取成无小数点文本");
    check(info.workerName == QStringLiteral("王五"), "负责人姓名应去掉首尾空白");
    check(info.workerId == QStringLiteral("42"), "整数字工号应取成 42");
    check(info.workerPhone.isEmpty(), "缺电话是可空，不是错误");
}

// 契约 5：坏输入一律给出原因
void testMalformedInput()
{
    struct Case { const char *json; };
    const Case cases[] = {
        {""},
        {"   "},
        {"施工信息"},
        {"[1,2,3]"},
        {"{\"construction_id\":"},
    };
    for (const Case &c : cases) {
        ConstructionInfo info;
        QString err;
        const bool accepted =
            ConstructionInfo::fromQrText(QString::fromUtf8(c.json), &info, &err);
        if (accepted) {
            throw std::runtime_error("坏输入被接受了");
        }
        if (err.isEmpty()) {
            throw std::runtime_error("坏输入被拒但没给原因（静默失败，界面上只会显示空白）");
        }
    }
}

// 契约 6：toJson 往返不丢未知键
void testJsonRoundTrip()
{
    ConstructionInfo info;
    QString err;
    const QString text = QStringLiteral(
        "{\"construction_id\":\"SG-9\",\"worker_name\":\"赵六\",\"team\":\"甲班\"}");
    check(ConstructionInfo::fromQrText(text, &info, &err), "带未知键的载荷应被接受");

    const QJsonObject out = info.toJson();
    check(out.value(QStringLiteral("team")).toString() == QStringLiteral("甲班"),
          "未知键必须原样透传（改字段时不用改存储与上行就是靠这条）");
    check(out.value(QStringLiteral("worker_phone")).isString(),
          "五个具名字段必须都出现（可空的以空串出现，详情框与对端才有统一的形状）");

    ConstructionInfo again;
    check(ConstructionInfo::fromJson(out, &again, &err), "toJson 的结果必须能再解回去");
    check(again.constructionId == info.constructionId && again.workerName == info.workerName,
          "往返后具名字段变了");
}

// 契约 7 + 8：会话状态机
void testSessionStateMachine()
{
    ConstructionSession &s = ConstructionSession::instance();
    s.logout();
    check(!s.isLoggedIn(), "初始/登出后不应是登录态");
    check(s.snapshot().isEmpty(), "登出后快照必须是空对象");

    QString err;
    check(s.startLogin(QStringLiteral("{\"construction_id\":\"SG-1\",\"worker_name\":\"张三\"}"), &err),
          "合法码应登录成功");
    check(s.isLoggedIn(), "登录成功后状态应为已登录");
    check(s.snapshot().workerName == QStringLiteral("张三"), "快照内容与登录时不一致");

    // 关键一条：失败的登录**不能**把已经在会话里的信息冲掉或冲成半成品
    err.clear();
    check(!s.startLogin(QStringLiteral("{\"worker_name\":\"李四\"}"), &err), "缺字段必须登录失败");
    check(s.isLoggedIn(), "登录失败后必须保持原来的登录态");
    check(s.snapshot().constructionId == QStringLiteral("SG-1"), "登录失败把会话内容改坏了");

    s.logout();
    check(!s.isLoggedIn(), "登出后应回到未登录");
    check(s.snapshot().isEmpty(), "登出后必须清空，否则下一条报警还会带上一班的名字");

    check(ConstructionInfo::display(QString()).compare(QString::fromUtf8("—")) == 0,
          "空值必须显示成破折号，不能留空白行");
    check(ConstructionInfo::display(QStringLiteral("张三")) == QStringLiteral("张三"),
          "有值时 display 不该改内容");
}

// 契约 9：离线解码（真解一张码）
void testDecodeFromFile(const QString &path, const QString &expectedWorker)
{
    std::vector<qrcode::QrHit> hits;
    QString err;
    if (!qrcode::decodeAvailable()) {
        std::printf("SKIP 解码用例（本次构建没有 ZBar）: %s\n", path.toUtf8().constData());
        return;
    }
    check(qrcode::scanQrFromImageFile(path, &hits, &err), "码图解码调用失败");
    check(!hits.empty(), "码图里应该解出至少一个码（光照/分辨率问题不在本用例范围，图是程序生成的）");
    check(hits.front().rect.width() > 0 && hits.front().rect.height() > 0,
          "命中矩形尺寸必须为正（结果框按它定中心点）");

    ConstructionSession &s = ConstructionSession::instance();
    s.logout();
    check(s.startLogin(hits.front().content, &err), "解出的原文应通过与扫码完全同一条登录路径");
    check(s.snapshot().workerName == expectedWorker, "登录后的负责人与码里不一致");
    s.logout();
}

// 契约 10：解不出的码给出原因而不是静默
void testBadQrRejected(const QString &path)
{
    if (!qrcode::decodeAvailable()) {
        return;
    }
    std::vector<qrcode::QrHit> hits;
    QString err;
    check(qrcode::scanQrFromImageFile(path, &hits, &err), "缺字段的码本身是能解出来的");
    if (hits.empty()) {
        throw std::runtime_error("缺字段的码解不出内容：这张测试图没起到它该起的作用");
    }
    ConstructionSession &s = ConstructionSession::instance();
    s.logout();
    err.clear();
    check(!s.startLogin(hits.front().content, &err), "缺施工id 的码必须停在登录页");
    check(!s.isLoggedIn(), "被拒后不能是登录态");
    check(quoted(err, "construction_id"), "提示必须点名缺的是施工id");
}

void testMissingFileGivesReason()
{
    std::vector<qrcode::QrHit> hits;
    QString err;
    if (!qrcode::decodeAvailable()) {
        return;
    }
    check(!qrcode::scanQrFromImageFile(QStringLiteral("/tmp/idge_definitely_not_here.png"), &hits, &err),
          "不存在的图片文件必须返回 false");
    check(!err.isEmpty(), "文件读不到也必须给原因，不能静默返回空结果");
}

} // namespace

int main(int argc, char **argv)
{
    QString qrPath = QStringLiteral("/tmp/idge_test_qr.png");
    QString badQrPath = QStringLiteral("/tmp/idge_test_qr_bad.png");
    QString expectedWorker = QStringLiteral("张三");
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (arg.contains(QStringLiteral("bad"))) {
            badQrPath = arg;
        } else {
            qrPath = arg;
        }
    }

    try {
        testFullPayload();
        std::puts("ok  1  完整载荷五字段各归其位");

        testRequiredFields();
        std::puts("ok  2/3  缺 construction_id / worker_name 都被拒且点名");

        testScalarCoercion();
        std::puts("ok  4  数字值与首尾空白处理正确");

        testMalformedInput();
        std::puts("ok  5  坏输入一律给出中文原因");

        testJsonRoundTrip();
        std::puts("ok  6  toJson 往返保留未知键");

        testSessionStateMachine();
        std::puts("ok  7/8  登录失败不动会话，登出清空");

        if (QFileInfo(qrPath).exists()) {
            testDecodeFromFile(qrPath, expectedWorker);
            std::puts("ok  9  离线解码 → 登录（真解了一张码）");
        } else {
            std::printf("SKIP 9  码图不存在：%s\n"
                        "      生成：python -m segno -o %s --scale 8 --border 4 "
                        "'<施工信息 JSON>'\n",
                        qrPath.toUtf8().constData(), qrPath.toUtf8().constData());
        }

        if (QFileInfo(badQrPath).exists()) {
            testBadQrRejected(badQrPath);
            std::puts("ok  10 缺字段的码：解得出内容、过不了校验");
        } else {
            std::printf("SKIP 10 缺字段的码图不存在：%s\n", badQrPath.toUtf8().constData());
        }

        testMissingFileGivesReason();
        std::puts("ok  11 图片读不到时给出原因而不是静默空结果");
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::puts("PASS test_construction_qr");
    return 0;
}
