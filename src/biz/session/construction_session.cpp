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
QString textOf(const QJsonObject &obj, const char *key)
{
    const QJsonValue v = obj.value(QLatin1String(key));
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

// 中文提示里同时给出界面用名与协议键名，现场照着改码的人不用猜
QString missingFieldText(const QString &uiName, const char *key)
{
    return QString::fromUtf8("二维码缺少字段 %1(%2)")
        .arg(uiName, QString::fromLatin1(key));
}

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
