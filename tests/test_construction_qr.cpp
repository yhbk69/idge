// ============================================================================
// test_construction_qr.cpp — 施工信息解析/校验/会话 + 二维码离线解码自检
// ----------------------------------------------------------------------------
// 契约：plan/qr-login-edge-v1.md（2026-11-04），载荷示例 plan/qr-login.json。
// 只链 Qt5::Core + Qt5::Gui + zbar + libcrypto + construction_session.cpp +
// qr_decoder.cpp：不开摄像头、不碰 GUI 线程、不读 config.json。
//
// 为什么值得单独钉：登录门的全部正确性都压在"从码里解出的原文 → 校验 → 会话"这一条
// 短链上，而它在板端只靠肉眼验证一次就会定型。缺字段被放过 = 未登录也能出报警；
// 校验写歪一个键名 = 现场每张码都登不上，且提示会说"缺的是另一个字段"。
//
// 覆盖十二组契约：
//   1) 完整载荷：person/plan 各字段归位、raw 原样留着、§5 计划窗口推导正确
//   2) 必填校验：人员 / 计划 / 人数三类缺法各给 §9 的中文提示，且一次只报第一条
//   3) 时间推导：starttime 显式值、constrainttime="4"、跨零点保护、startmorrow、多日期取第一
//   4) 标量处理：数字年份/人数、18 位 taskId 不丢精度、首尾空白
//   5) 坏输入：空 / 非 JSON / 数组 / 截断 → 一律拒绝并给原因
//   6) toJson：写成嵌套形状、未知键透传、旧扁平键不再出现在新行里
//   7) 会话：失败**不动**当前会话；登出清空；summary 只带 taskId + 姓名
//   8) fields()：新形状按中文名与固定顺序渲染、计划时间另起一行、历史形状仍可显示
//   9) 历史兼容：taskIdFromJsonText 读两代形状；旧扁平载荷不能再用来登录
//  10) 离线解码：真解一张新契约码 → 同一条 startLogin 路径通过
//  11) 缺字段的码：解得出内容、过不了校验，提示点名缺的是人员信息
//  12) HAS_ZBAR 没编进来 / 图片读不到时给出原因，而不是静默返回空结果
//
// §6.4 有效期与 §6.5 验签**本轮不做**（2026-11-04 裁定），所以没有对应用例；
// issuedAt/expiresAt/signature 仍要求被原样解析保存（第 1 组断言 raw 保留了它们）。
//
// 第 10/11 条需要码图（缺了就 SKIP）。图是用 plan/qr-login.json 原样生成的 M 级码，
// 等价于现场张贴的码。
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

using session::ConstructionField;
using session::ConstructionInfo;
using session::ConstructionSession;
using session::LoginPolicy;

namespace {

void check(bool ok, const char *message)
{
    if (!ok) {
        throw std::runtime_error(message);
    }
}

bool quoted(const QString &text, const QString &needle)
{
    return text.contains(needle);
}

QJsonObject obj(const QString &json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        throw std::runtime_error("测试用的 JSON 字面量写错了");
    }
    return doc.object();
}

// plan/qr-login.json 的最小化副本（值一字不差，保证第 10 组的签名向量对得上）
QString sampleJson()
{
    return QStringLiteral(
        "{\"v\":1,"
        "\"issuedAt\":\"2026-11-04T20:00:00+08:00\","
        "\"expiresAt\":\"2027-12-31T23:59:59+08:00\","
        "\"person\":{\"id\":\"16016\",\"name\":\"张三\",\"role\":\"SITE_LEAD\"},"
        "\"plan\":{\"taskId\":\"202601211641114927023\",\"lineName\":\"5号线\","
        "\"stationName\":\"宋家庄\",\"workType\":\"轨行区综合检修\",\"year\":2026,"
        "\"month\":11,\"days\":[\"04\"],\"tendays\":1,\"starttime\":\"\","
        "\"startmorrow\":false,\"constrainttime\":\"4\",\"endtime\":\"03:30\","
        "\"endmorrow\":true,\"totalPersonCount\":15}}");
}

// 1
void testFullPayload()
{
    ConstructionInfo info;
    QString err;
    check(ConstructionInfo::fromQrText(sampleJson(), &info, &err), "联调码应当被接受");
    check(err.isEmpty(), "接受时不该带原因");

    check(info.personId == QStringLiteral("16016"), "person.id 取值错误");
    check(info.personName == QStringLiteral("张三"), "person.name 取值错误");
    check(info.personRole == QStringLiteral("SITE_LEAD"), "person.role 取值错误");
    check(info.taskId == QStringLiteral("202601211641114927023"), "plan.taskId 取值错误");
    check(info.lineName == QStringLiteral("5号线"), "线名取值错误");
    check(info.stationName == QStringLiteral("宋家庄"), "车站取值错误");
    check(info.workType == QStringLiteral("轨行区综合检修"), "作业内容取值错误");
    check(info.year == 2026 && info.month == 11, "年月取值错误");
    check(info.days.size() == 1 && info.days.first() == QStringLiteral("04"), "days 应取第一个值");
    check(info.tendays == 1, "tendays 取值错误");
    check(info.constrainttime == QStringLiteral("4"), "constrainttime 取值错误");
    check(info.endtime == QStringLiteral("03:30"), "endtime 取值错误");
    check(info.endmorrow, "endmorrow 取值错误");
    check(info.totalPersonCount == 15, "计划人数取值错误");
    // 示例文件里还有一个协议文档没写的 "v":1（§1 说"协议版本不写入二维码"），
    // 按未知键透传：不认它、也不因为它拒码。
    check(info.raw.keys().size() == 5, "raw 没原样留下顶层五个键（含 v）");

    // §5.2 的示例：2026-11-04 23:00 → 2026-11-05 03:30（+08:00）
    check(info.planWindowText() == QStringLiteral("2026-11-04 23:00 → 2026-11-05 03:30"),
          "计划窗口推导与 §5.2 示例不一致");
    check(info.startsAt.toString(Qt::ISODate) == QStringLiteral("2026-11-04T23:00:00+08:00"),
          "开始时间必须带 +08:00");
    check(!info.isEmpty(), "有内容的对象不应判为空");
}

// 2
void testRequiredFields()
{
    ConstructionInfo info;
    QString err;

    // 缺 person.id
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"name\":\"李四\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"endtime\":\"01:00\","
              "\"totalPersonCount\":3}}"), &info, &err),
          "缺人员工号必须拒绝登录");
    check(quoted(err, QStringLiteral("缺少人员信息")), "缺人员字段的提示必须是『缺少人员信息』");

    // 缺 person.name
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"endtime\":\"01:00\","
              "\"totalPersonCount\":3}}"), &info, &err),
          "缺姓名必须拒绝登录");
    check(quoted(err, QStringLiteral("缺少人员信息")), "缺姓名的提示同样是『缺少人员信息』");

    // 缺 plan.taskId
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"李四\"},\"plan\":{\"year\":2026,"
              "\"month\":11,\"days\":[\"04\"],\"endtime\":\"01:00\",\"totalPersonCount\":3}}"),
              &info, &err),
          "缺 plan.taskId 必须拒绝登录");
    check(quoted(err, QStringLiteral("缺少施工计划")), "缺计划字段的提示必须是『缺少施工计划』");

    // 人数 <= 0
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"李四\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"endtime\":\"01:00\","
              "\"totalPersonCount\":0}}"), &info, &err),
          "计划人数 0 必须拒绝登录");
    check(quoted(err, QStringLiteral("计划人数无效")), "人数提示必须是『计划人数无效』");

    // 一次只报第一条：人员和计划都缺 → 先报人员
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral("{}"), &info, &err), "空对象必须拒绝登录");
    check(quoted(err, QStringLiteral("缺少人员信息")), "多个字段都缺时只报第一条（人员）");

    // 只有空白的姓名等价于缺字段
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"   \"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"endtime\":\"01:00\","
              "\"totalPersonCount\":3}}"), &info, &err),
          "只有空白的姓名必须拒绝登录");
}

// 3
void testTimeWindow()
{
    ConstructionInfo info;
    QString err;

    // starttime 显式给出时优先于 constrainttime
    check(ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"starttime\":\"22:30\","
              "\"constrainttime\":\"4\",\"endtime\":\"02:00\",\"totalPersonCount\":5}}"),
              &info, &err),
          "显式 starttime 的码应当被接受");
    check(info.startsAt.toString(QStringLiteral("HH:mm")) == QStringLiteral("22:30"),
          "starttime 必须覆盖 constrainttime");

    // starttime 空 + constrainttime 不是 "4" → 无法得到开始时间
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"starttime\":\"\","
              "\"constrainttime\":\"2\",\"endtime\":\"02:00\",\"totalPersonCount\":5}}"),
              &info, &err),
          "无法得到开始时间的码必须被拒");
    check(quoted(err, QStringLiteral("施工计划时间无效")), "时间推导失败的提示必须是『施工计划时间无效』");

    // 跨零点保护：end <= start 且 endmorrow=false → 结束日期自动 +1 天
    check(ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"starttime\":\"22:00\","
              "\"endtime\":\"01:00\",\"endmorrow\":false,\"totalPersonCount\":5}}"),
              &info, &err),
          "跨零点保护应当让码有效");
    check(info.endsAt.date() == QDate(2026, 11, 5), "结束日期必须自动跨到次日");

    // startmorrow：开始日期 +1 天（结束也给 endmorrow=true，否则结束日期比开始更早）
    check(ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"starttime\":\"01:00\","
              "\"startmorrow\":true,\"endtime\":\"05:00\",\"endmorrow\":true,"
              "\"totalPersonCount\":5}}"), &info, &err),
          "startmorrow 的码应当被接受");
    check(info.startsAt.date() == QDate(2026, 11, 5), "开始日期必须顺延到次日");

    // days 多个值只取第一个非空（§5.3）
    check(ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"\",\"06\",\"07\"],"
              "\"starttime\":\"23:00\",\"endtime\":\"01:00\",\"totalPersonCount\":5}}"),
              &info, &err),
          "多日期的码应当被接受（只取第一个非空值）");
    check(info.days.first() == QStringLiteral("06") && info.startsAt.date() == QDate(2026, 11, 6),
          "多日期必须取第一个非空值");

    // 非法日期（2 月 30 日）→ 时间无效
    err.clear();
    check(!ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":2,\"days\":[\"30\"],\"starttime\":\"23:00\","
              "\"endtime\":\"01:00\",\"totalPersonCount\":5}}"), &info, &err),
          "不存在的日期必须被拒");
}

// 4
void testScalarCoercion()
{
    ConstructionInfo info;
    QString err;
    const QString text = QStringLiteral(
        "{\"person\":{\"id\":16016,\"name\":\"  王五  \"},\"plan\":{\"taskId\":"
        "\"202601211641114927023\",\"year\":2026,\"month\":11,\"days\":[\"04\"],"
        "\"starttime\":\"23:00\",\"endtime\":\"01:00\",\"totalPersonCount\":\"7\"}}");
    check(ConstructionInfo::fromQrText(text, &info, &err), "数字值不应被拒");
    check(info.personId == QStringLiteral("16016"), "数字人员工号应取成无小数点文本");
    check(info.personName == QStringLiteral("王五"), "姓名应去掉首尾空白");
    // 18 位雪花 id 必须原样保留：QString::number(double) 会打成科学计数法并丢精度
    check(info.taskId == QStringLiteral("202601211641114927023"), "18 位 taskId 精度丢了");
    check(info.totalPersonCount == 7, "人数写成字符串也应读出来");
    check(info.personRole.isEmpty(), "缺 role 是可空，不是错误");
}

// 5
void testMalformedInput()
{
    struct Case { const char *json; };
    const Case cases[] = {
        {""},
        {"   "},
        {"施工信息"},
        {"[1,2,3]"},
        {"{\"person\":{"},
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
    // §9：JSON 解析失败统一提示"二维码内容无法识别"
    ConstructionInfo info;
    QString err;
    ConstructionInfo::fromQrText(QStringLiteral("施工信息"), &info, &err);
    check(quoted(err, QStringLiteral("二维码内容无法识别")), "解析失败的提示必须与 §9 一致");
}

// 6
void testJsonRoundTrip()
{
    ConstructionInfo info;
    QString err;
    QJsonObject src = obj(sampleJson());
    QJsonObject plan = src.value(QStringLiteral("plan")).toObject();
    plan.insert(QStringLiteral("team"), QStringLiteral("甲班"));   // 未知子键
    src.insert(QStringLiteral("plan"), plan);
    src.insert(QStringLiteral("extra"), QStringLiteral("透传"));

    check(ConstructionInfo::fromQrText(
              QString::fromUtf8(QJsonDocument(src).toJson(QJsonDocument::Compact)), &info, &err),
          "带未知键的载荷应被接受");

    const QJsonObject out = info.toJson();
    check(out.value(QStringLiteral("extra")).toString() == QStringLiteral("透传"),
          "未知顶层键必须原样透传");
    check(out.value(QStringLiteral("plan")).toObject().value(QStringLiteral("team")).toString()
              == QStringLiteral("甲班"),
          "未知 plan 子键必须原样透传");
    check(out.value(QStringLiteral("person")).toObject().value(QStringLiteral("name")).toString()
              == QStringLiteral("张三"),
          "toJson 必须写出嵌套的 person");
    check(!out.contains(QStringLiteral("construction_id")),
          "旧扁平键不该出现在新行里（读得进、写不出）");
    check(out.value(QStringLiteral("plan")).toObject().value(QStringLiteral("days")).isArray(),
          "days 必须以数组写出");

    ConstructionInfo again;
    check(ConstructionInfo::fromJson(out, &again, &err), "toJson 的结果必须能再解回去");
    check(again.taskId == info.taskId && again.personName == info.personName
              && again.startsAt == info.startsAt,
          "往返后具名字段或推导窗口变了");
}

// 7
void testSessionStateMachine()
{
    ConstructionSession &s = ConstructionSession::instance();
    s.setLoginPolicy(LoginPolicy());   // 联调档，不受其它用例影响
    s.logout();
    check(!s.isLoggedIn(), "初始/登出后不应是登录态");
    check(s.snapshot().isEmpty(), "登出后快照必须是空对象");

    QString err;
    check(s.startLogin(sampleJson(), &err), "合法码应登录成功");
    check(s.isLoggedIn(), "登录成功后状态应为已登录");
    check(s.snapshot().personName == QStringLiteral("张三"), "快照内容与登录时不一致");
    check(s.summary() == QStringLiteral("施工 202601211641114927023 / 负责人 张三"),
          "summary 口径变了（日志与角标都读它）");

    // 关键一条：失败的登录**不能**把已经在会话里的信息冲掉或冲成半成品
    err.clear();
    check(!s.startLogin(QStringLiteral("{\"person\":{\"name\":\"李四\"}}"), &err),
          "缺字段必须登录失败");
    check(s.isLoggedIn(), "登录失败后必须保持原来的登录态");
    check(s.snapshot().taskId == QStringLiteral("202601211641114927023"),
          "登录失败把会话内容改坏了");

    s.logout();
    check(!s.isLoggedIn(), "登出后应回到未登录");
    check(s.snapshot().isEmpty(), "登出后必须清空，否则下一条报警还会带上一班的名字");
    check(s.summary() == QString::fromUtf8("未登录"), "登出后的 summary 必须是未登录");

    check(ConstructionInfo::display(QString()).compare(QString::fromUtf8("—")) == 0,
          "空值必须显示成破折号，不能留空白行");
}

// 8
void testDisplayFields()
{
    const QVector<ConstructionField> rows = ConstructionInfo::fields(obj(sampleJson()));
    // 3 个 person 键 + 14 个 plan 键 + 1 行推导窗口 + 示例里那个未写进协议的 v = 19 行
    check(rows.size() == 19, "新形状的显示行数不对（人员 3 + 计划 14 + 计划时间 1 + 未知键 v）");

    check(rows.at(0).label == QString::fromUtf8("人员工号")
              && rows.at(0).value == QStringLiteral("16016"),
          "第一行必须是人员工号");
    check(rows.at(1).label == QString::fromUtf8("姓名"), "第二行必须是姓名");
    check(rows.at(3).label == QString::fromUtf8("施工计划(taskId)"),
          "人员三行之后紧接着是施工计划");
    check(rows.at(rows.size() - 2).label == QString::fromUtf8("计划时间")
              && rows.at(rows.size() - 2).value == QStringLiteral("2026-11-04 23:00 → 2026-11-05 03:30"),
          "推导窗口必须紧接在计划各键之后，且与 §5.2 一致");
    check(rows.last().label == QStringLiteral("v") && rows.last().value == QStringLiteral("1"),
          "未知顶层键 v 必须原样排在最后（协议文档没写它，但示例文件带了）");

    // 已知键为空也要成行（如 role 缺省时显示 "—"），不能让"没这个字段"和"空着"看起来一样
    ConstructionInfo info;
    QString err;
    check(ConstructionInfo::fromQrText(QStringLiteral(
              "{\"person\":{\"id\":\"1\",\"name\":\"张三\"},\"plan\":{\"taskId\":\"T1\","
              "\"year\":2026,\"month\":11,\"days\":[\"04\"],\"starttime\":\"23:00\","
              "\"endtime\":\"01:00\",\"totalPersonCount\":5}}"), &info, &err),
          "渲染用例的载荷本身应能登录");
    const QVector<ConstructionField> bare = ConstructionInfo::fields(info.toJson());
    check(bare.at(2).label == QString::fromUtf8("角色") && bare.at(2).value == QString::fromUtf8("—"),
          "缺省的角色必须渲染成破折号");

    // 旧扁平形状（历史库）：仍按中文名渲染
    const QJsonObject legacy = obj(QStringLiteral(
        "{\"construction_id\":\"SG-1\",\"worker_name\":\"张三\","
        "\"worker_phone\":\"13800001111\"}"));
    const QVector<ConstructionField> legacyRows = ConstructionInfo::fields(legacy);
    check(legacyRows.size() == 3, "旧扁平行应当渲染出三项");
    check(legacyRows.at(0).label == QString::fromUtf8("施工id（旧）"),
          "历史行的第一项必须是施工id");

    check(ConstructionInfo::fields(QJsonObject()).isEmpty(), "空对象不该渲染出任何行");
    check(ConstructionInfo::fieldsFromJson(QString()).isEmpty(), "空 JSON 文本渲染成空表");
    check(ConstructionInfo::fieldsFromJson(QStringLiteral("not json")).isEmpty(),
          "解不开的 JSON 文本渲染成空表而不是崩");
}

// 9
void testLegacyCompatibility()
{
    // 新形状
    check(ConstructionInfo::taskIdFromJsonText(
              QString::fromUtf8(QJsonDocument(obj(sampleJson())).toJson(QJsonDocument::Compact)))
              == QStringLiteral("202601211641114927023"),
          "taskIdFromJsonText 必须读 plan.taskId");
    // 旧形状（历史库里的行）
    check(ConstructionInfo::taskIdFromJsonText(
              QStringLiteral("{\"construction_id\":\"SG-2026-11\",\"worker_name\":\"张三\"}"))
              == QStringLiteral("SG-2026-11"),
          "taskIdFromJsonText 必须回落旧键 construction_id");
    check(ConstructionInfo::taskIdFromJsonText(QString()).isEmpty(), "空文本必须返回空串");
    check(ConstructionInfo::taskIdFromJsonText(QStringLiteral("[]")).isEmpty(),
          "非对象必须返回空串");

    // 旧扁平载荷**不能再用来登录**：新契约要求 person.id，历史行只用于显示与 taskId
    ConstructionInfo info;
    QString err;
    check(!ConstructionInfo::fromQrText(
              QStringLiteral("{\"construction_id\":\"SG-1\",\"worker_name\":\"张三\"}"),
              &info, &err),
          "旧扁平载荷必须被拒（缺 person.id）");
    check(quoted(err, QStringLiteral("缺少人员信息")), "旧扁平载荷的拒绝原因必须是缺人员信息");
}

// 10
// §6.6 是本轮唯一保留的校验：代码里写死"设备绑定到某个计划"的那条路。
// 其余交给服务端的判断（过期、验签）本轮不做，见文件头。
void testExpectedTaskId()
{
    ConstructionInfo info;
    QString err;

    LoginPolicy bound;
    bound.expectedTaskId = QStringLiteral("OTHER-TASK");
    err.clear();
    check(!ConstructionInfo::fromQrText(sampleJson(), &info, &err, bound),
          "taskId 与本机当前计划不一致必须被拒");
    check(quoted(err, QStringLiteral("与当前设备任务不一致")),
          "计划不一致的提示必须与 §9 一致");
    check(ConstructionInfo::fromQrText(sampleJson(), &info, &err, LoginPolicy()),
          "默认策略（不比对）不该拒绝合法码");

    LoginPolicy matched;
    matched.expectedTaskId = QStringLiteral("202601211641114927023");
    check(ConstructionInfo::fromQrText(sampleJson(), &info, &err, matched),
          "taskId 一致时必须放行");
}

// 11
void testDecodeFromFile(const QString &path)
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
    s.setLoginPolicy(LoginPolicy());
    s.logout();
    check(s.startLogin(hits.front().content, &err), "解出的原文应通过与扫码完全同一条登录路径");
    check(s.snapshot().personName == QStringLiteral("张三"), "登录后的姓名与码里不一致");
    check(s.snapshot().taskId == QStringLiteral("202601211641114927023"),
          "登录后的 taskId 与码里不一致");
    s.logout();
}

// 12
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
    s.setLoginPolicy(LoginPolicy());
    s.logout();
    err.clear();
    check(!s.startLogin(hits.front().content, &err), "缺 person.id 的码必须停在登录页");
    check(!s.isLoggedIn(), "被拒后不能是登录态");
    check(quoted(err, QStringLiteral("缺少人员信息")), "提示必须点名缺的是人员信息");
}

// 13
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
        std::puts("ok  1  完整载荷 + §5.2 计划窗口推导");

        testRequiredFields();
        std::puts("ok  2  必填校验按 §9 提示，一次只报第一条");

        testTimeWindow();
        std::puts("ok  3  时间推导：constrainttime / 跨零点 / startmorrow / 多日期");

        testScalarCoercion();
        std::puts("ok  4  数字值与首尾空白处理正确，18 位 taskId 不丢精度");

        testMalformedInput();
        std::puts("ok  5  坏输入一律给出中文原因");

        testJsonRoundTrip();
        std::puts("ok  6  toJson 写成嵌套形状且透传未知键");

        testSessionStateMachine();
        std::puts("ok  7  登录失败不动会话，登出清空");

        testDisplayFields();
        std::puts("ok  8  逐键渲染：中文名 + 固定顺序 + 计划时间行 + 历史形状");

        testLegacyCompatibility();
        std::puts("ok  9  历史库：taskId 读两代形状，旧载荷不能再登录");

        testExpectedTaskId();
        std::puts("ok  10 taskId 与本机当前计划比对（§6.6）");

        if (QFileInfo(qrPath).exists()) {
            testDecodeFromFile(qrPath);
            std::puts("ok  11 离线解码 → 登录（真解了一张新契约码）");
        } else {
            std::printf("SKIP 11 码图不存在：%s\n", qrPath.toUtf8().constData());
        }

        if (QFileInfo(badQrPath).exists()) {
            testBadQrRejected(badQrPath);
            std::puts("ok  12 缺人员信息的码：解得出内容、过不了校验");
        } else {
            std::printf("SKIP 12 缺字段的码图不存在：%s\n", badQrPath.toUtf8().constData());
        }

        testMissingFileGivesReason();
        std::puts("ok  13 图片读不到时给出原因而不是静默空结果");
    } catch (const std::exception &e) {
        std::printf("FAIL: %s\n", e.what());
        return 1;
    }

    std::puts("PASS test_construction_qr");
    return 0;
}
