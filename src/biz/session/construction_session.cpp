/**
 * @file construction_session.cpp
 * @brief 施工信息的解析/校验实现 + 登录会话状态（读写锁保护）
 *
 * 所属模块：src/biz/session（详见同目录 README.md）。这里是"扫码登录解出的一段
 * JSON 文本"进入本机的**唯一**入口：解析与校验只写在 fromJson 一处，
 * 登录页、"测试登录"、单测都走它，口径不分叉。
 *
 * 契约：plan/qr-login-edge-v1.md（2026-11-04），载荷示例 plan/qr-login.json。
 * 形状是 `person{}` + `plan{}` 的嵌套对象；旧扁平形状只作为历史库的**读取兼容**。
 *
 * 本文件的两块内容刻意分开：
 *   - ConstructionInfo：纯值类型，无状态、无锁，负责"码里的文本 → 字段契约"；
 *   - ConstructionSession：进程内单例，只保存"当前会话值"这一份状态。
 *
 * 线程模型：写只在 UI 线程（startLogin / logout），读会来自解码线程
 * （AlarmManager::makeAlarmRecord 给每条报警打戳时调 snapshot()）。
 * snapshot() 按值返回、函数返回即释放读锁，调用方不得持锁去写数据库。
 *
 * 依赖：只用 Qt Core（QString/QJson/QDateTime）。
 * 不 include 任何 src/ui / src/base 头，因此 tests/test_construction_qr.cpp 能离线跑。
 */
#include "construction_session.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTime>
#include <QReadLocker>
#include <QWriteLocker>

namespace session {

namespace {

// 契约固定的时区偏移（qr-login-edge-v1.md §5："时区统一为 Asia/Shanghai"）。
// 故意不用 QTimeZone("Asia/Shanghai")：那要读系统 tzdata，板子上少一份 zoneinfo
// 就会让登录时间悄悄错 8 小时；固定 +08:00 与规约同义且不依赖外部文件。
constexpr int kPlanUtcOffsetSeconds = 8 * 3600;

// 二维码里的值可能是字符串，也可能是数字（年份/人数常写成 number）。
// 统一取成去掉首尾空白的文本；非字符串标量走 JSON 文本表示，对象/数组视为空。
/**
 * @brief 任意 JSON 标量 → 显示/存储用的文本
 * @return 去首尾空白后的文本；null / 对象 / 数组返回空串（**不**展开成 JSON 文本）
 *
 * 之所以必须按类型分支：`QJsonValue::toString()` 对数字和布尔一律返回空串，
 * 直接用它的话码里写成 number 的工号会在详情框里凭空消失。三条特判各有一次具体取舍：
 *   - bool → "1"/"0"，与界面其它开关类显示一致，不留 "true"/"false" 这种英文串；
 *   - 整值 double 走 qint64 分支：QString::number(double) 默认 'g' 格式只保 6 位有效数字，
 *     雪花型 id（如 18 位 taskId）会被打成科学计数法且**精度丢失**，落库后就再也查不回来；
 *   - 对象/数组当空：具名字段只该是标量。嵌套结构仍完整留在 raw 里，
 *     toJson() 原样透传未知键，所以"视为空"只影响具名字段的显示，不丢数据。
 */
QString valueText(const QJsonValue &v)
{
    QString s;
    switch (v.type()) {
    case QJsonValue::String:
        s = v.toString();
        break;
    case QJsonValue::Bool:
        s = v.toBool() ? QStringLiteral("1") : QStringLiteral("0");
        break;
    case QJsonValue::Double: {
        const double d = v.toDouble();
        if (d == static_cast<double>(static_cast<qint64>(d)))
            s = QString::number(static_cast<qint64>(d));
        else
            s = QString::number(d);
        break;
    }
    default:
        break;
    }
    return s.trimmed();
}

QString textOf(const QJsonObject &obj, const char *key)
{
    return valueText(obj.value(QLatin1String(key)));
}

// 数字字段：JSON number 与"数字字符串"都收（现场签发系统两种都出现过）。
int intOf(const QJsonObject &obj, const char *key)
{
    const QJsonValue v = obj.value(QLatin1String(key));
    if (v.isDouble())
        return static_cast<int>(v.toDouble());
    return valueText(v).toInt();
}

// 布尔字段：只认 JSON bool。缺失/false 同义，字符串 "true" 不认（§4.3 定的是 boolean）。
bool boolOf(const QJsonObject &obj, const char *key)
{
    return obj.value(QLatin1String(key)).toBool(false);
}

// days 只取第一个非空值（§5.3），其余值一概不看——多日语义未定，不自行拼窗口。
QString firstDayOf(const QJsonObject &plan)
{
    const QJsonArray arr = plan.value(QStringLiteral("days")).toArray();
    for (const QJsonValue &v : arr) {
        const QString s = valueText(v);
        if (!s.isEmpty())
            return s;
    }
    return QString();
}

// 把码里的对象读进结构体：**不做任何校验**，缺键留空。
// fromJson 与 fields() 共用这一份读法，所以"哪个键叫什么"不会在显示与校验之间分叉。
// 旧扁平形状（历史库里的 construction_info）在这里被翻译成同样的字段：
// construction_id → taskId、worker_name → personName，其余旧键只留在 raw 里供显示。
void readObject(const QJsonObject &obj, ConstructionInfo *info)
{
    info->raw = obj;

    const QJsonObject person = obj.value(QStringLiteral("person")).toObject();
    const QJsonObject plan = obj.value(QStringLiteral("plan")).toObject();

    info->issuedAt = textOf(obj, "issuedAt");
    info->expiresAt = textOf(obj, "expiresAt");
    info->signatureAlg = textOf(obj, "signatureAlg");
    info->signature = textOf(obj, "signature");

    info->personId = textOf(person, "id");
    info->personName = textOf(person, "name");
    info->personRole = textOf(person, "role");

    info->taskId = textOf(plan, "taskId");
    info->lineName = textOf(plan, "lineName");
    info->stationName = textOf(plan, "stationName");
    info->workType = textOf(plan, "workType");
    info->year = intOf(plan, "year");
    info->month = intOf(plan, "month");
    info->days.clear();
    const QString day = firstDayOf(plan);
    if (!day.isEmpty())
        info->days << day;
    info->tendays = intOf(plan, "tendays");
    info->starttime = textOf(plan, "starttime");
    info->startmorrow = boolOf(plan, "startmorrow");
    info->constrainttime = textOf(plan, "constrainttime");
    info->endtime = textOf(plan, "endtime");
    info->endmorrow = boolOf(plan, "endmorrow");
    info->totalPersonCount = intOf(plan, "totalPersonCount");

    // 历史形状：只补计划身份与负责人姓名，其它旧字段不映射（新契约里没有它们）
    if (info->taskId.isEmpty())
        info->taskId = textOf(obj, "construction_id");
    if (info->personName.isEmpty())
        info->personName = textOf(obj, "worker_name");
}

/**
 * @brief §5 的计划窗口推导
 * @return false = 时间字段不合法（调用方统一报"二维码施工计划时间无效"）
 *
 * 规则逐条对应 §5.1：基准日期 = year/month/days[0]；开始时间取 starttime，
 * 空则 constrainttime=="4" → 23:00；结束时间必填；endmorrow 加一天；
 * 结束不晚于开始且 endmorrow==false 时自动跨零点一天；最后必须 end > start。
 * 推导只在登录那一刻做一次，结果存进 startsAt/endsAt——后续所有使用方（详情框、
 * 将来的计划作用域）都读这两个值，不各自重算。
 */
bool deriveWindow(ConstructionInfo *info)
{
    const int day = info->days.isEmpty() ? 0 : info->days.first().toInt();
    const QDate base(info->year, info->month, day);
    if (!base.isValid())
        return false;

    QTime startClock;
    if (!info->starttime.isEmpty())
        startClock = QTime::fromString(info->starttime, QStringLiteral("HH:mm"));
    else if (info->constrainttime == QLatin1String("4"))
        startClock = QTime(23, 0);   // 运营结束后
    if (!startClock.isValid())
        return false;

    const QTime endClock = QTime::fromString(info->endtime, QStringLiteral("HH:mm"));
    if (!endClock.isValid())
        return false;

    const QDate startDate = info->startmorrow ? base.addDays(1) : base;
    info->startsAt = QDateTime(startDate, startClock, Qt::OffsetFromUTC, kPlanUtcOffsetSeconds);

    QDate endDate = base;
    if (info->endmorrow)
        endDate = endDate.addDays(1);
    QDateTime end(endDate, endClock, Qt::OffsetFromUTC, kPlanUtcOffsetSeconds);
    if (!info->endmorrow && end <= info->startsAt)
        end = end.addDays(1);   // 跨零点保护，只加一天（§5.1 第 3 条）
    info->endsAt = end;

    return info->startsAt.isValid() && info->endsAt.isValid()
           && info->endsAt > info->startsAt;
}

// 中文提示里同时给出界面用名与协议键名，现场照着改码的人不用猜
QString missingFieldText(const QString &uiName, const QString &key)
{
    return QString::fromUtf8("二维码缺少字段 %1(%2)").arg(uiName, key);
}

// 已知键的界面中文名与显示顺序。只描述"界面上叫什么、排第几"，
// 键名本身仍只在 readObject 里出现一次。
struct KeyLabel {
    const char *key;
    const char *label;   // UTF-8
};

const KeyLabel kPersonFields[] = {
    {"id",    "人员工号"},
    {"name",  "姓名"},
    {"role",  "角色"},
};

const KeyLabel kPlanFields[] = {
    {"taskId",           "施工计划(taskId)"},
    {"lineName",         "线路"},
    {"stationName",      "车站"},
    {"workType",         "作业内容"},
    {"year",             "计划年份"},
    {"month",            "计划月份"},
    {"days",             "计划日期"},
    {"tendays",          "旬"},
    {"starttime",        "开始时间"},
    {"startmorrow",      "开始顺延次日"},
    {"constrainttime",   "开始时间约束"},
    {"endtime",          "结束时间"},
    {"endmorrow",        "结束顺延次日"},
    {"totalPersonCount", "计划人数"},
};

const KeyLabel kLegacyFields[] = {
    {"construction_id", "施工id（旧）"},
    {"worker_name",     "负责人（旧）"},
    {"worker_phone",    "负责人电话（旧）"},
    {"worker_id",       "负责人工号（旧）"},
    {"work_content",    "施工内容（旧）"},
};

bool isKnownTopLevel(const QString &key)
{
    return key == QLatin1String("person") || key == QLatin1String("plan")
        || key == QLatin1String("issuedAt") || key == QLatin1String("expiresAt")
        || key == QLatin1String("signatureAlg") || key == QLatin1String("signature");
}

} // namespace

bool ConstructionInfo::isEmpty() const
{
    return personId.isEmpty() && personName.isEmpty() && personRole.isEmpty()
        && taskId.isEmpty() && lineName.isEmpty() && stationName.isEmpty()
        && workType.isEmpty() && raw.isEmpty();
}

// 施工计划身份的唯一取值判据：新形状读 plan.taskId，旧扁平形状回落 construction_id。
// DAO 写 task_id 列、启动回填老行、Pad 出数三处都从这里走。
QString ConstructionInfo::taskIdFromJsonText(const QString &jsonText)
{
    if (jsonText.isEmpty()) {
        return QString();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(jsonText.toUtf8());
    if (!doc.isObject()) {
        return QString();
    }
    ConstructionInfo info;
    readObject(doc.object(), &info);
    return info.taskId;
}

QJsonObject ConstructionInfo::toJson() const
{
    // 先铺未知的顶层键（协议先加字段时不丢），已知键一律用结构体里的值覆盖。
    QJsonObject obj;
    for (auto it = raw.begin(); it != raw.end(); ++it) {
        if (!isKnownTopLevel(it.key()))
            obj.insert(it.key(), it.value());
    }

    if (!issuedAt.isEmpty())     obj.insert(QStringLiteral("issuedAt"), issuedAt);
    if (!expiresAt.isEmpty())    obj.insert(QStringLiteral("expiresAt"), expiresAt);
    if (!signatureAlg.isEmpty()) obj.insert(QStringLiteral("signatureAlg"), signatureAlg);
    if (!signature.isEmpty())    obj.insert(QStringLiteral("signature"), signature);

    QJsonObject person = raw.value(QStringLiteral("person")).toObject();
    person.insert(QStringLiteral("id"), personId);
    person.insert(QStringLiteral("name"), personName);
    person.insert(QStringLiteral("role"), personRole);
    obj.insert(QStringLiteral("person"), person);

    QJsonObject plan = raw.value(QStringLiteral("plan")).toObject();
    plan.insert(QStringLiteral("taskId"), taskId);
    plan.insert(QStringLiteral("lineName"), lineName);
    plan.insert(QStringLiteral("stationName"), stationName);
    plan.insert(QStringLiteral("workType"), workType);
    plan.insert(QStringLiteral("year"), year);
    plan.insert(QStringLiteral("month"), month);
    QJsonArray dayArray;
    for (const QString &d : days)
        dayArray.append(d);
    plan.insert(QStringLiteral("days"), dayArray);
    plan.insert(QStringLiteral("tendays"), tendays);
    plan.insert(QStringLiteral("starttime"), starttime);
    plan.insert(QStringLiteral("startmorrow"), startmorrow);
    plan.insert(QStringLiteral("constrainttime"), constrainttime);
    plan.insert(QStringLiteral("endtime"), endtime);
    plan.insert(QStringLiteral("endmorrow"), endmorrow);
    plan.insert(QStringLiteral("totalPersonCount"), totalPersonCount);
    obj.insert(QStringLiteral("plan"), plan);

    return obj;
}

bool ConstructionInfo::fromJson(const QJsonObject &obj, ConstructionInfo *out, QString *err,
                                const LoginPolicy &policy)
{
    if (!out)
        return false;

    ConstructionInfo info;
    readObject(obj, &info);

    // ---- §6 第 2 步：必填 ----
    if (info.personId.isEmpty() || info.personName.isEmpty()) {
        if (err) *err = QString::fromUtf8("二维码缺少人员信息");
        return false;
    }
    if (info.taskId.isEmpty()) {
        if (err) *err = QString::fromUtf8("二维码缺少施工计划");
        return false;
    }
    if (info.totalPersonCount <= 0) {
        if (err) *err = QString::fromUtf8("二维码计划人数无效");
        return false;
    }

    // ---- §6 第 3 步：时间可推导 ----
    if (!deriveWindow(&info)) {
        if (err) *err = QString::fromUtf8("二维码施工计划时间无效");
        return false;
    }

    // §6 第 4 步（有效期/签发时钟）与第 5 步（验签）**本轮不做**（2026-11-04 裁定）。
    // issuedAt/expiresAt/signatureAlg/signature 仍照原样解析并保存（§8 要求会话保留它们），
    // 只是不据此拒绝登录。要做时在这里补两段，提示文案见规约 §9。

    // ---- §6 第 6 步：与本机当前计划比对 ----
    if (!policy.expectedTaskId.isEmpty() && info.taskId != policy.expectedTaskId) {
        if (err) *err = QString::fromUtf8("二维码施工计划与当前设备任务不一致");
        return false;
    }

    *out = info;
    return true;
}

bool ConstructionInfo::fromQrText(const QString &qrText, ConstructionInfo *out, QString *err,
                                  const LoginPolicy &policy)
{
    const QString text = qrText.trimmed();
    if (text.isEmpty()) {
        if (err) *err = QString::fromUtf8("二维码内容为空");
        return false;
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(text.toUtf8(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        // 只回类型，不把原文塞进日志/界面（码里可能是别的系统的内容）
        if (err) *err = QString::fromUtf8("二维码内容无法识别");
        return false;
    }

    return fromJson(doc.object(), out, err, policy);
}

QString ConstructionInfo::display(const QString &value)
{
    return value.isEmpty() ? QStringLiteral("—") : value;
}

QString ConstructionInfo::planWindowText() const
{
    if (!startsAt.isValid() || !endsAt.isValid())
        return QString();
    return QStringLiteral("%1 → %2")
        .arg(startsAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
             endsAt.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
}

QVector<ConstructionField> ConstructionInfo::fields(const QJsonObject &obj)
{
    QVector<ConstructionField> rows;
    if (obj.isEmpty()) {
        return rows;
    }

    const bool nested = obj.contains(QStringLiteral("person"))
                        || obj.contains(QStringLiteral("plan"));
    QStringList emitted;

    auto append = [&rows, &emitted](const QString &label, const QString &key,
                                    const QString &value) {
        ConstructionField row;
        row.label = label;
        row.value = display(value);
        rows.append(row);
        emitted << key;
    };

    if (nested) {
        const QJsonObject person = obj.value(QStringLiteral("person")).toObject();
        for (const KeyLabel &f : kPersonFields) {
            const QString key = QString::fromLatin1(f.key);
            if (!person.contains(key))
                continue;
            append(QString::fromUtf8(f.label), QStringLiteral("person.") + key,
                   valueText(person.value(key)));
        }

        const QJsonObject plan = obj.value(QStringLiteral("plan")).toObject();
        for (const KeyLabel &f : kPlanFields) {
            const QString key = QString::fromLatin1(f.key);
            if (!plan.contains(key))
                continue;
            append(QString::fromUtf8(f.label), QStringLiteral("plan.") + key,
                   valueText(plan.value(key)));
        }

        // 推导窗口单独一行：它是 §5 算出来的，不是码里的某个键，所以放在键行之后
        ConstructionInfo info;
        readObject(obj, &info);
        if (deriveWindow(&info)) {
            ConstructionField row;
            row.label = QString::fromUtf8("计划时间");
            row.value = info.planWindowText();
            rows.append(row);
        }
    } else {
        // 历史扁平形状：老库里的行，仍按中文名显示
        for (const KeyLabel &f : kLegacyFields) {
            const QString key = QString::fromLatin1(f.key);
            if (!obj.contains(key))
                continue;
            append(QString::fromUtf8(f.label), key, valueText(obj.value(key)));
        }
    }

    // 码里多出来的键也照原样列出来（用键名当标签），这样协议加字段时
    // 界面先能看到值，再决定要不要给它一个中文名。QJsonObject::keys() 按键排序，显示稳定。
    const QStringList rest = obj.keys();
    for (const QString &key : rest) {
        if (isKnownTopLevel(key))
            continue;
        if (emitted.contains(key))
            continue;
        append(key, key, valueText(obj.value(key)));
    }

    if (nested) {
        const QStringList groups = {QStringLiteral("person"), QStringLiteral("plan")};
        for (const QString &group : groups) {
            const QJsonObject sub = obj.value(group).toObject();
            for (auto it = sub.begin(); it != sub.end(); ++it) {
                const QString path = group + QLatin1Char('.') + it.key();
                if (emitted.contains(path))
                    continue;
                append(path, path, valueText(it.value()));
            }
        }
    }

    return rows;
}

QVector<ConstructionField> ConstructionInfo::fieldsFromJson(const QString &json)
{
    if (json.trimmed().isEmpty()) {
        return QVector<ConstructionField>();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (!doc.isObject()) {
        return QVector<ConstructionField>();
    }
    return fields(doc.object());
}

ConstructionSession &ConstructionSession::instance()
{
    static ConstructionSession s;
    return s;
}

void ConstructionSession::setLoginPolicy(const LoginPolicy &policy)
{
    QWriteLocker locker(&lock_);
    policy_ = policy;
}

LoginPolicy ConstructionSession::loginPolicy() const
{
    QReadLocker locker(&lock_);
    return policy_;
}

bool ConstructionSession::startLogin(const QString &qrText, QString *err)
{
    ConstructionInfo info;
    if (!ConstructionInfo::fromQrText(qrText, &info, err, loginPolicy()))
        return false;
    return startLogin(info);
}

bool ConstructionSession::startLogin(const ConstructionInfo &info)
{
    QWriteLocker locker(&lock_);
    info_ = info;
    loggedIn_ = true;
    return true;
}

void ConstructionSession::logout()
{
    QWriteLocker locker(&lock_);
    info_ = ConstructionInfo{};
    loggedIn_ = false;
}

// loggedIn_ 与 info_ 由同一把写锁一起改，所以两者不会分叉。但判"有没有登录"要看这个
// 标志而不是 snapshot().isEmpty()：startLogin(ConstructionInfo) 不校验必填，
// 已登录但字段全空的状态是可能存在的，两种判法会给出相反答案。
bool ConstructionSession::isLoggedIn() const
{
    QReadLocker locker(&lock_);
    return loggedIn_;
}

ConstructionInfo ConstructionSession::snapshot() const
{
    QReadLocker locker(&lock_);
    return info_;
}

QString ConstructionSession::summary() const
{
    const ConstructionInfo info = snapshot();
    if (info.taskId.isEmpty() || info.personName.isEmpty())
        return QString::fromUtf8("未登录");
    return QString::fromUtf8("施工 %1 / 负责人 %2")
        .arg(ConstructionInfo::display(info.taskId),
             ConstructionInfo::display(info.personName));
}

} // namespace session
