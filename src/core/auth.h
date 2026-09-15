#pragma once

#include <QString>
#include <QRecursiveMutex>
#include <memory>

class Beatport;

struct TokenPair {
    QString accessToken;
    QString refreshToken;
    qint64 expiresIn = 0;
    QString tokenType;
    QString scope;
    QString loginId;
    qint64 issuedAt = 0;
};

// OAuth 端点（与 Go internal/beatport/auth.go 常量一致）。
extern const QString kClientId;
extern const QString kTokenEndpoint;
extern const QString kAuthorizeEndpoint;
extern const QString kLoginEndpoint;

// Beatport OAuth：login（sessionid cookie）→ authorize（302 code）→ issue token。
class Auth {
public:
    Auth(QString username, QString password, QString cacheFile);

    QString cacheFile() const { return m_cacheFile; }
    void setCredentials(const QString &username, const QString &password);

    bool loadCache(QString *err);
    bool writeCache(QString *err) const;

    // Check：token 过期前 300s 刷新；刷新失败重新 Init；无 token 直接报错。
    bool check(Beatport *bp, QString *err);
    void invalidate();
    bool init(Beatport *bp, QString *err);

    // fetch() 用：当前 access token（空串=无 token，不发 Authorization）。
    QString accessToken() const;
    bool hasToken() const;
    QString loginIdHash() const;

private:
    bool refresh(Beatport *bp, QString *err);
    bool issue(Beatport *bp, const QString &code, QString *err);
    bool authorize(Beatport *bp, const QString &sessionId, QString *code, QString *err);
    bool login(Beatport *bp, QString *sessionId, QString *err);

    QString m_username;
    QString m_password;
    QString m_cacheFile;
    std::shared_ptr<TokenPair> m_token;
    mutable QRecursiveMutex m_mutex;
};
