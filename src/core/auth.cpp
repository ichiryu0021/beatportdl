#include "auth.h"

#include "beatport.h"
#include "util.h"
#include "logger.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMap>
#include <QMutexLocker>
#include <QNetworkCookie>
#include <QUrl>
#include <QUrlQuery>

// OAuth 端点常量（与 Go internal/beatport/auth.go 完全一致）。
const QString kClientId =
    QStringLiteral("ryZ8LuyQVPqbK2mBX2Hwt4qSMtnWuTYSqBPO92yQ");
const QString kTokenEndpoint = QStringLiteral("/auth/o/token/");
const QString kAuthorizeEndpoint =
    QStringLiteral("/auth/o/authorize/?client_id=") + kClientId
    + QStringLiteral("&response_type=code");
const QString kLoginEndpoint = QStringLiteral("/auth/login/");

namespace {

// 解码 token JSON（对应 Go json.Decoder.Decode(tokenPair)）。
TokenPair parseTokenPair(const QByteArray &body, QString *parseErr)
{
    TokenPair out;
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (parseErr)
            *parseErr = pe.errorString();
        return out;
    }
    const QJsonObject o = doc.object();
    out.accessToken = o.value(QStringLiteral("access_token")).toString();
    out.refreshToken = o.value(QStringLiteral("refresh_token")).toString();
    out.expiresIn = static_cast<qint64>(
        o.value(QStringLiteral("expires_in")).toDouble());
    out.tokenType = o.value(QStringLiteral("token_type")).toString();
    out.scope = o.value(QStringLiteral("scope")).toString();
    out.loginId = o.value(QStringLiteral("login_id")).toString();
    out.issuedAt = static_cast<qint64>(
        o.value(QStringLiteral("issued_at")).toDouble());
    return out;
}

// JSON 字符串字面量转义（encoding/json 的 HTML-safe 转义对 token 无影响，
// 这里实现标准 JSON 转义即可，字段顺序由调用方固定）。
QByteArray jsonQuote(const QString &s)
{
    QByteArray out = "\"";
    for (QChar c : s) {
        switch (c.unicode()) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c.unicode() < 0x20) {
                out += QStringLiteral("\\u%1")
                           .arg(static_cast<unsigned int>(c.unicode()), 4, 16,
                                QLatin1Char('0'))
                           .toLatin1();
            } else {
                out += QString(c).toUtf8();
            }
        }
    }
    out += '"';
    return out;
}

// 与 json.MarshalIndent(t, "", " ") 输出一致：字段按 Go 结构体顺序、
// 单空格缩进、无尾随换行。
QByteArray marshalTokenPair(const TokenPair &t)
{
    QByteArray out = "{\n";
    out += " \"access_token\": " + jsonQuote(t.accessToken) + ",\n";
    out += " \"refresh_token\": " + jsonQuote(t.refreshToken) + ",\n";
    out += " \"expires_in\": " + QByteArray::number(t.expiresIn) + ",\n";
    out += " \"token_type\": " + jsonQuote(t.tokenType) + ",\n";
    out += " \"scope\": " + jsonQuote(t.scope) + ",\n";
    out += " \"login_id\": " + jsonQuote(t.loginId) + ",\n";
    out += " \"issued_at\": " + QByteArray::number(t.issuedAt) + "\n";
    out += "}";
    return out;
}

} // namespace

Auth::Auth(QString username, QString password, QString cacheFile)
    : m_username(std::move(username))
    , m_password(std::move(password))
    , m_cacheFile(std::move(cacheFile))
{
}

void Auth::setCredentials(const QString &username, const QString &password)
{
    QMutexLocker lock(&m_mutex);
    m_username = username;
    m_password = password;
}

QString Auth::accessToken() const
{
    QMutexLocker lock(&m_mutex);
    return m_token ? m_token->accessToken : QString();
}

bool Auth::hasToken() const
{
    QMutexLocker lock(&m_mutex);
    return m_token != nullptr;
}

QString Auth::loginIdHash() const
{
    QMutexLocker lock(&m_mutex);
    return fnv64aHex(QStringLiteral("%1:%2").arg(m_username, m_password));
}

bool Auth::loadCache(QString *err)
{
    QFile f(m_cacheFile);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err)
            *err = QStringLiteral("failed to read token file: %1")
                       .arg(f.errorString());
        return false;
    }
    const QByteArray data = f.readAll();
    f.close();

    QString parseErr;
    TokenPair loaded = parseTokenPair(data, &parseErr);
    if (!parseErr.isEmpty()) {
        if (err)
            *err = QStringLiteral("failed to unmarshal token data: %1")
                       .arg(parseErr);
        return false;
    }

    if (loaded.loginId != loginIdHash()) {
        if (err)
            *err = QStringLiteral("login id does not match");
        return false;
    }

    {
        QMutexLocker lock(&m_mutex);
        m_token = std::make_shared<TokenPair>(std::move(loaded));
    }
    return true;
}

bool Auth::writeCache(QString *err) const
{
    QMutexLocker lock(&m_mutex);
    if (!m_token) {
        if (err)
            *err = QStringLiteral("failed to marshal tokenPair: no token");
        return false;
    }
    const QByteArray data = marshalTokenPair(*m_token);

    // 对应 os.MkdirAll(path.Dir(cacheFile), 0700)：仅对新建目录收紧权限。
    const QString dir = QFileInfo(m_cacheFile).absolutePath();
    const bool dirExisted = QFileInfo::exists(dir);
    if (!QDir().mkpath(dir)) {
        if (err)
            *err = QStringLiteral("could not create folder for cache file: %1")
                       .arg(dir);
        return false;
    }
    if (!dirExisted) {
        QFile(dir).setPermissions(
            QFileDevice::ReadOwner | QFileDevice::WriteOwner
            | QFileDevice::ExeOwner);
    }

    QFile f(m_cacheFile);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = QStringLiteral("failed to write token to cache: %1")
                       .arg(f.errorString());
        return false;
    }
    const qint64 written = f.write(data);
    const bool flushOk = f.flush();
    f.close();
    if (written != data.size() || !flushOk) {
        if (err)
            *err = QStringLiteral("failed to write token to cache: %1")
                       .arg(f.errorString());
        return false;
    }
    // 对应 0600：仅属主可读写（Windows 下设置 owner-only ACE）。
    f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    return true;
}

bool Auth::check(Beatport *bp, QString *err)
{
    // Go：先 RLock 读 token/过期时间；需要刷新时 Lock 包住 refresh/init。
    QMutexLocker lock(&m_mutex);
    const qint64 currentTime = QDateTime::currentSecsSinceEpoch();
    if (!m_token) {
        if (err)
            *err = QStringLiteral("not authenticated: no cached token");
        return false;
    }
    const qint64 tokenExpiration = m_token->issuedAt + m_token->expiresIn;
    if (currentTime + 300 < tokenExpiration)
        return true;

    logInfo(QStringLiteral("Refreshing token"));
    QString refreshErr;
    if (!refresh(bp, &refreshErr)) {
        QString initErr;
        if (!init(bp, &initErr)) {
            if (err)
                *err = QStringLiteral("invalid token and authorization error: %1")
                           .arg(initErr);
            return false;
        }
    }
    return true;
}

void Auth::invalidate()
{
    QMutexLocker lock(&m_mutex);
    if (m_token)
        m_token->issuedAt = 0;
}

bool Auth::init(Beatport *bp, QString *err)
{
    logInfo(QStringLiteral("Logging in"));

    QString sessionId;
    QString stepErr;
    if (!login(bp, &sessionId, &stepErr)) {
        if (err)
            *err = stepErr;
        return false;
    }

    QString code;
    if (!authorize(bp, sessionId, &code, &stepErr)) {
        if (err)
            *err = QStringLiteral("authorize: %1").arg(stepErr);
        return false;
    }

    if (!issue(bp, code, &stepErr)) {
        if (err)
            *err = QStringLiteral("issue token: %1").arg(stepErr);
        return false;
    }
    return true;
}

bool Auth::refresh(Beatport *bp, QString *err)
{
    if (!m_token) {
        if (err)
            *err = QStringLiteral("not authenticated: no cached token");
        return false;
    }
    const QMap<QString, QString> payload{
        {QStringLiteral("client_id"), kClientId},
        {QStringLiteral("refresh_token"), m_token->refreshToken},
        {QStringLiteral("grant_type"), QStringLiteral("refresh_token")},
    };

    QByteArray body;
    if (!bp->fetchForm(QStringLiteral("POST"), kTokenEndpoint, payload,
                       &body, err))
        return false;

    QString parseErr;
    TokenPair response = parseTokenPair(body, &parseErr);
    if (!parseErr.isEmpty()) {
        if (err)
            *err = parseErr;
        return false;
    }
    const QString oldLoginId = m_token->loginId;
    m_token = std::make_shared<TokenPair>(std::move(response));
    m_token->issuedAt = QDateTime::currentSecsSinceEpoch();
    m_token->loginId = oldLoginId;
    return writeCache(err);
}

bool Auth::issue(Beatport *bp, const QString &code, QString *err)
{
    QMap<QString, QString> payload{
        {QStringLiteral("client_id"), kClientId},
    };
    if (!code.isEmpty()) {
        payload.insert(QStringLiteral("grant_type"),
                       QStringLiteral("authorization_code"));
        payload.insert(QStringLiteral("code"), code);
    } else {
        payload.insert(QStringLiteral("grant_type"),
                       QStringLiteral("password"));
        payload.insert(QStringLiteral("username"), m_username);
        payload.insert(QStringLiteral("password"), m_password);
    }

    QByteArray body;
    if (!bp->fetchForm(QStringLiteral("POST"), kTokenEndpoint, payload,
                       &body, err))
        return false;

    QString parseErr;
    TokenPair response = parseTokenPair(body, &parseErr);
    if (!parseErr.isEmpty()) {
        if (err)
            *err = parseErr;
        return false;
    }
    m_token = std::make_shared<TokenPair>(std::move(response));
    m_token->issuedAt = QDateTime::currentSecsSinceEpoch();
    m_token->loginId = loginIdHash();
    return writeCache(err);
}

bool Auth::authorize(Beatport *bp, const QString &sessionId, QString *code,
                     QString *err)
{
    // Go：临时在 client headers 上挂 cookie，请求后删除。
    bp->m_headers.insert(QByteArray("cookie"),
                         QStringLiteral("sessionid=%1").arg(sessionId).toUtf8());
    QByteArray body;
    QByteArray location;
    QString fetchErr;
    const bool ok = bp->fetchInternal(
        QByteArray("GET"), kAuthorizeEndpoint, QByteArray(), QByteArray(),
        &body, &location, nullptr, &fetchErr);
    bp->m_headers.remove(QByteArray("cookie"));
    if (!ok) {
        if (err)
            *err = fetchErr;
        return false;
    }

    // 解析 302 Location 查询串中的 code（query.Get 语义：解码后取值）。
    const QUrl redirectUrl(QString::fromUtf8(location), QUrl::TolerantMode);
    const QUrlQuery query(redirectUrl);
    const QString value = query.queryItemValue(QStringLiteral("code"),
                                               QUrl::FullyDecoded);
    if (!value.isEmpty()) {
        *code = value;
        return true;
    }
    if (err)
        *err = QStringLiteral("invalid authorization code");
    return false;
}

bool Auth::login(Beatport *bp, QString *sessionId, QString *err)
{
    QJsonObject payload;
    payload.insert(QStringLiteral("username"), m_username);
    payload.insert(QStringLiteral("password"), m_password);

    QByteArray body;
    QList<QNetworkCookie> cookies;
    if (!bp->fetchJson(QStringLiteral("POST"), kLoginEndpoint, payload,
                       &body, nullptr, &cookies, err))
        return false;

    for (const QNetworkCookie &cookie : cookies) {
        if (cookie.name() == QByteArray("sessionid")) {
            *sessionId = QString::fromUtf8(cookie.value());
            return true;
        }
    }
    if (err)
        *err = QStringLiteral("invalid session cookie");
    return false;
}
