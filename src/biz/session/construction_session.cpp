#include "construction_session.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QReadLocker>
#include <QWriteLocker>

namespace session {

const char ConstructionInfo::kKeyConstructionId[] = "construction_id";
const char ConstructionInfo::kKeyWorkerName[]     = "worker_name";
const char ConstructionInfo::kKeyWorkerPhone[]    = "worker_phone";
const char ConstructionInfo::kKeyWorkerId[]       = "worker_id";
const char ConstructionInfo::kKeyWorkContent[]    = "work_content";

namespace {

// 二维码里的值可能是字符串，也可能是数字（工号/id 常被写成 number）。
// 统一取成去掉首尾空白的文本；非字符串标量走 JSON 文本表示，对象/数组视为空。
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
    return valueText(obj.value(QString::fromLatin1(key)));
}

// 中文提示里同时给出界面用名与协议键名，现场照着改码的人不用猜
QString missingFieldText(const QString &uiName, const char *key)
{
    return QString::fromUtf8("二维码缺少字段 %1(%2)")
        .arg(uiName, QString::fromLatin1(key));
}

// 已知键的界面中文名与显示顺序。键名取自 ConstructionInfo::kKey*，
// 所以"协议里叫什么"仍然只有一个出处，这里只额外承担"界面上叫什么、排第几"。
// 新增字段时在这里加一行，两个详情框就各多一行，别处不用动。
struct KeyLabel {
    const char *key;
    const char *label;   // UTF-8
};

const KeyLabel kPreferredFields[] = {
    {ConstructionInfo::kKeyConstructionId, "施工id"},
    {ConstructionInfo::kKeyWorkerName,     "负责人"},
    {ConstructionInfo::kKeyWorkerPhone,    "负责人电话"},
    {ConstructionInfo::kKeyWorkerId,       "负责人工号"},
    {ConstructionInfo::kKeyWorkContent,    "施工内容"},
};

} // namespace

bool ConstructionInfo::isEmpty() const
{
    return constructionId.isEmpty() && workerName.isEmpty() && workerPhone.isEmpty() &&
           workerId.isEmpty() && workContent.isEmpty() && raw.isEmpty();
}

QJsonObject ConstructionInfo::toJson() const
{
    QJsonObject obj = raw;
    obj.insert(QLatin1String(kKeyConstructionId), constructionId);
    obj.insert(QLatin1String(kKeyWorkerName), workerName);
    obj.insert(QLatin1String(kKeyWorkerPhone), workerPhone);
    obj.insert(QLatin1String(kKeyWorkerId), workerId);
    obj.insert(QLatin1String(kKeyWorkContent), workContent);
    return obj;
}

bool ConstructionInfo::fromJson(const QJsonObject &obj, ConstructionInfo *out, QString *err)
{
    if (!out)
        return false;

    ConstructionInfo info;
    info.raw = obj;
    info.constructionId = textOf(obj, kKeyConstructionId);
    info.workerName     = textOf(obj, kKeyWorkerName);
    info.workerPhone    = textOf(obj, kKeyWorkerPhone);
    info.workerId       = textOf(obj, kKeyWorkerId);
    info.workContent    = textOf(obj, kKeyWorkContent);

    // 必填校验：缺哪个就点名哪个，一次只报第一个（现场按提示补一项即可）
    if (info.constructionId.isEmpty()) {
        if (err) *err = missingFieldText(QString::fromUtf8("施工id"), kKeyConstructionId);
        return false;
    }
    if (info.workerName.isEmpty()) {
        if (err) *err = missingFieldText(QString::fromUtf8("负责人姓名"), kKeyWorkerName);
        return false;
    }

    *out = info;
    return true;
}

bool ConstructionInfo::fromQrText(const QString &qrText, ConstructionInfo *out, QString *err)
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
        if (err) {
            *err = doc.isObject()
                ? QString::fromUtf8("二维码内容不是有效 JSON：%1")
                      .arg(parseError.errorString())
                : QString::fromUtf8("二维码内容不是施工信息 JSON 对象");
        }
        return false;
    }

    return fromJson(doc.object(), out, err);
}

QString ConstructionInfo::display(const QString &value)
{
    return value.isEmpty() ? QStringLiteral("—") : value;
}

QVector<ConstructionField> ConstructionInfo::fields(const QJsonObject &obj)
{
    QVector<ConstructionField> rows;
    if (obj.isEmpty()) {
        return rows;
    }

    QStringList emitted;
    for (const KeyLabel &f : kPreferredFields) {
        const QString key = QString::fromLatin1(f.key);
        if (!obj.contains(key)) {
            continue;
        }
        ConstructionField row;
        row.label = QString::fromUtf8(f.label);
        row.value = display(valueText(obj.value(key)));
        rows.append(row);
        emitted << key;
    }

    // 码里多出来的键也照原样列出来（用键名当标签），这样协议加字段时
    // 界面先能看到值，再决定要不要给它一个中文名。QJsonObject::keys() 按键排序，显示稳定。
    const QStringList rest = obj.keys();
    for (const QString &key : rest) {
        if (emitted.contains(key)) {
            continue;
        }
        ConstructionField row;
        row.label = key;
        row.value = display(valueText(obj.value(key)));
        rows.append(row);
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

bool ConstructionSession::startLogin(const QString &qrText, QString *err)
{
    ConstructionInfo info;
    if (!ConstructionInfo::fromQrText(qrText, &info, err))
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
    if (info.constructionId.isEmpty() || info.workerName.isEmpty())
        return QString::fromUtf8("未登录");
    return QString::fromUtf8("施工 %1 / 负责人 %2")
        .arg(ConstructionInfo::display(info.constructionId),
             ConstructionInfo::display(info.workerName));
}

} // namespace session
