/**
 * @file construction_session.cpp
 * @brief 施工信息的解析/校验实现 + 登录会话状态（读写锁保护）
 *
 * 所属模块：src/biz/session（详见同目录 README.md）。这里是"扫码登录解出的一段
 * JSON 文本"进入本机的**唯一**入口：解析与校验只写在 fromJson 一处，
 * 登录页、"测试登录"、单测都走它，口径不分叉。
 *
 * 本文件的两块内容刻意分开：
 *   - ConstructionInfo：纯值类型，无状态、无锁，负责"码里的文本 → 字段契约"；
 *   - ConstructionSession：进程内单例，只保存"当前会话值"这一份状态。
 *
 * 线程模型：写只在 UI 线程（startLogin / logout），读会来自解码线程
 * （AlarmManager::makeAlarmRecord 给每条报警打戳时调 snapshot()）。
 * snapshot() 按值返回、函数返回即释放读锁，调用方不得持锁去写数据库。
 *
 * 依赖：只用 Qt Core（QString/QJsonObject/QJsonDocument），不 include 任何 src/ui 头，
 * 因此 tests/test_construction_qr.cpp 能离线跑 11 条契约。
 */
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
/**
 * @brief 任意 JSON 标量 → 显示/存储用的文本
 * @return 去首尾空白后的文本；null / 对象 / 数组返回空串（**不**展开成 JSON 文本）
 *
 * 之所以必须按类型分支：`QJsonValue::toString()` 对数字和布尔一律返回空串，
 * 直接用它的话码里写成 number 的工号会在详情框里凭空消失。三条特判各有一次具体取舍：
 *   - bool → "1"/"0"，与界面其它开关类显示一致，不留 "true"/"false" 这种英文串；
 *   - 整值 double 走 qint64 分支：QString::number(double) 默认 'g' 格式只保 6 位有效数字，
 *     雪花型 id（如 19 位施工 id）会被打成科学计数法且**精度丢失**，落库后就再也查不回来；
 *   - 对象/数组当空：具名字段只该是标量。嵌套结构仍完整留在 raw 里，
 *     toJson() 原样落库，所以"视为空"只影响具名字段的显示，不丢数据。
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

// 空对象的判据把 raw 也算进来：码里只带了未知键（协议先发字段、结构体还没跟上）时
// 这份信息**不是**空的——落库与详情框都要靠 raw 把它显示出来。
bool ConstructionInfo::isEmpty() const
{
    return constructionId.isEmpty() && workerName.isEmpty() && workerPhone.isEmpty() &&
           workerId.isEmpty() && workContent.isEmpty() && raw.isEmpty();
}

/**
 * @brief 落库与上行用的规范化 JSON
 * @return raw 的副本，上面覆盖五个具名字段（键名恒定存在，值为空时是空串而不是缺键）
 *
 * 为什么先铺 raw 再覆盖：未知键必须原样透传（见 session/README.md 的"改字段只动四处"），
 * 而具名字段可能被用户/上层改写过，以结构体为准。
 *
 * 已知副作用：具名字段解出来是空串时，这里会**用空串覆盖掉 raw 里的原值**。
 * 只有两种成因——上层手工构造了 ConstructionInfo 又没填该字段，或码里把该键写成了
 * 对象/数组（valueText 视为空）。后者会丢掉那段嵌套内容，遇到就先看码而不是改这里。
 */
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

/**
 * @brief 解析 + 校验：全工程**唯一**的校验规则所在
 * @param obj 码里解出的 JSON 对象；未知键不报错也不拒绝（协议先加字段时的兼容路径）
 * @param out 出参，nullptr 时直接返回 false（不抛、不崩）
 * @param err 可选出参；非空时写入中文原因， nullptr 时跳过文案构造
 * @return 两个必填键都非空才 true；false 时 out 不被写（不留半成品对象）
 *
 * 必填只有 construction_id 与 worker_name，且**一次只报第一个**缺的字段——
 * 现场照着提示补一项即可，不用一次看懂一整段错误。
 * 判"缺"的标准是 valueText 归一化之后为空：数字 0、空串、纯空白、对象都算缺。
 * 登录页 / "测试登录" / 单测都从这里过，别在调用方再加一遍校验（会分叉）。
 */
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

/**
 * @brief 二维码原文 → ConstructionInfo（登录页唯一的调用入口）
 * @return false 的三种情况：原文空白 / 不是合法 JSON / 是 JSON 但不是对象（数组、标量），
 *         后两类都到不了 fromJson，因此不会报"缺字段"
 *
 * 入参先 trimmed()：扫码枪与 ZBar 常在原文两端带换行，不去掉会让 QJsonDocument
 * 直接解析失败，现场表现成"码是对的但解不开"。
 * 失败原因刻意**不带原文**（见函数体注释）——码内容可能是别的系统的载荷，
 * 进日志等于把别人的数据抄进本机日志。
 */
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

// 空值统一显示 "—"（em dash）而不是留白：详情框与列表里空白会被读成"渲染坏了"，
// 而历史上确实没有施工信息（老库补列后是 NULL → 读成空串）就是这种情况。
QString ConstructionInfo::display(const QString &value)
{
    return value.isEmpty() ? QStringLiteral("—") : value;
}

/**
 * @brief 逐键渲染成显示行（两个详情框共用这一个函数）
 * @return 空对象返回空表；否则"已知键按 kPreferredFields 的顺序 + 其余键按键名字典序"
 *
 * 两段输出的语义不同是刻意的：已知键带中文名、位置固定，未知键直接用键名当标签排后面，
 * 于是"协议加了字段"先表现为界面上多一行裸键名，再决定要不要给它中文名——
 * 不需要为了让界面不出乱码而改代码。
 * QJsonObject::keys() 本身按键字典序返回，所以后半段的顺序稳定、截图对比不会 flaky。
 * 值都过 display()：空串显示 "—"，0 与 false 这类标量已被 valueText 转成文本。
 */
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

/**
 * @brief 落库/上行里那段 JSON 文本 → fields()
 * @return 空串、解析失败、不是对象——一律返回**空表**，不报错、不写日志
 *
 * 静默是设计要求：这条路径的入参来自 `alarms.construction_info` 列，老行是 NULL→空串，
 * 手工改坏过的库也可能是半截 JSON；详情框不该因为一条历史数据显示成错误态。
 * 判据用 trimmed().isEmpty() 而不是 isEmpty()：库里存过带换行的紧凑文本时结果一致。
 */
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

// Meyers 单例：C++11 起静态局部变量的初始化本身线程安全，所以第一次调用来自
// UI 线程还是解码线程都不会出问题；实例内所有状态由 lock_ 保护。
ConstructionSession &ConstructionSession::instance()
{
    static ConstructionSession s;
    return s;
}

/**
 * @brief 扫码登录：解析 + 校验 + 定死为当前会话内容（三步一次做完）
 * @param err 可选出参，透传给 fromQrText
 * @return false 时**会话完全不动**——不存在"半登录"状态，也不需要调用方回滚
 *
 * 两个登录入口（扫码 / "测试登录"手输 JSON）都走这一个重载，校验规则因此只有一份。
 */
bool ConstructionSession::startLogin(const QString &qrText, QString *err)
{
    ConstructionInfo info;
    if (!ConstructionInfo::fromQrText(qrText, &info, err))
        return false;
    return startLogin(info);
}

/**
 * @brief 以已解析好的对象登录（写锁保护的真正赋值处）
 * @param info 调用方应保证已通过 fromJson/fromQrText 校验；本函数**不再校验**必填字段
 * @return 恒为 true（写内存状态不会失败）
 *
 * 前置条件不在这里检查是刻意的：校验只有 fromJson 一处，"必填没填也算登录成功"这种
 * 口径分叉宁可不要。绕过 fromQrText 直接调它（单测常用）就能造出一个
 * constructionId 为空的登录态，此时标题栏角标与上行的 taskId 都会是空串。
 *
 * "登录状态下再扫码不静默改写归属"这条规则也不在这里判——门禁在调用方
 * （src/ui/form/frmmain.cpp 用 isLoggedIn() 决定放行主界面），这里再判一次就是两处口径。
 */
bool ConstructionSession::startLogin(const ConstructionInfo &info)
{
    QWriteLocker locker(&lock_);
    info_ = info;
    loggedIn_ = true;
    return true;
}

/**
 * @brief 清空会话（整份替换成空对象，不逐字段清）
 *
 * 调用方**必须配对**做两件事，本函数都不替它做：
 *   1. AlarmManager::resetThrottles() —— 限流窗口是本机内存 map，不清的话重登后
 *      2 秒（类别）/10 秒（围栏）窗口内的第一条报警会被静默吞掉，现象是
 *      "换了个人扫码，头一条报警没了"（见 src/biz/alarm/README.md）；
 *   2. 回到登录页并挡住主界面门禁。
 * 已产生并落库的报警不受影响：施工信息在产生时就固化进记录，退出登录不回改历史行，
 * 仍在重传队列里的那条带的仍是它产生时的负责人。
 */
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

/**
 * @brief 取当前会话的施工信息快照
 * @return 按值拷贝；未登录时是空对象（isEmpty() 为真），不是 nullptr 之类的特殊值
 *
 * 读锁只在函数体内持有，返回即释放——这是"解码线程可以给报警打戳、又不会持锁跨
 * 数据库写"的实现方式（session/README.md 的线程与锁一节）。
 * 拷贝代价是 QJsonObject 的一次深拷贝（几个短字符串），每条报警一次，实测不构成热点；
 * 想改成共享指针会牺牲"调用方拿到的一定是不再变化的快照"这条性质。
 * 写只在 UI 线程，读可来自任意线程；QReadWriteLock 默认非递归，别在持读锁时再进 snapshot()。
 */
ConstructionInfo ConstructionSession::snapshot() const
{
    QReadLocker locker(&lock_);
    return info_;
}

/**
 * @brief 一行摘要，供日志与详情显示
 * @return 两个必填任缺其一就返回"未登录"；否则 "施工 <id> / 负责人 <name>"
 *
 * 刻意只取两个必填字段（不含电话/工号/内容）：这行文本会进日志，
 * 摘要里带上手机号等于把个人信息抄进日志文件。
 * 内部走 snapshot() 取快照，不直接读 info_，因此不会绕过锁。
 */
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
