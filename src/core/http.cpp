#include "http.h"

#include <QAtomicInt>
#include <QCoreApplication>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QNetworkProxy>
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QDir>

namespace {

// ---- 进程内全局代理状态（唯一真源）-----------------------------------------

QMutex &proxyMutex()
{
    static QMutex m;
    return m;
}

ProxySetting &proxyState()
{
    static ProxySetting s; // 默认 Auto
    return s;
}

QAtomicInt &proxyVersion()
{
    static QAtomicInt v{1};
    return v;
}

// 把一份 ProxySetting 应用到 QNAM（在 QNAM 所属线程内调用）。
void applyProxyToNam(QNetworkAccessManager *nam, const ProxySetting &s)
{
    switch (s.mode) {
    case ProxyMode::Off:
        nam->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));
        break;
    case ProxyMode::Auto:
        // DefaultProxy → 回退 applicationProxy()，其默认值来自系统代理配置。
        nam->setProxy(QNetworkProxy(QNetworkProxy::DefaultProxy));
        break;
    case ProxyMode::Manual: {
        const QUrl pu(s.url);
        QNetworkProxy proxy;
        if (pu.scheme().startsWith(QStringLiteral("socks")))
            proxy.setType(QNetworkProxy::Socks5Proxy);
        else
            proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(pu.host());
        proxy.setPort(static_cast<quint16>(pu.port(0)));
        if (!pu.userName().isEmpty())
            proxy.setUser(pu.userName());
        if (!pu.password().isEmpty())
            proxy.setPassword(pu.password());
        nam->setProxy(proxy);
        break;
    }
    }
}

// 所有出网通道共用的线程局部 QNAM：版本号变化时在本线程重配代理。
QNetworkAccessManager *sharedNam()
{
    thread_local QNetworkAccessManager *nam = nullptr;
    thread_local int appliedVersion = 0;
    if (!nam) {
        nam = new QNetworkAccessManager();
        // 故意不 delete：线程退出/应用关闭顺序不可控时让 OS 回收，
        // 避免 QNetworkAccessManager 在 QCoreApplication 之后析构崩溃。
        appliedVersion = 0;
    }
    const int cur = proxyVersion().loadAcquire();
    if (appliedVersion != cur) {
        applyProxyToNam(nam, globalProxy());
        appliedVersion = cur;
    }
    return nam;
}

} // namespace

void setGlobalProxy(const ProxySetting &setting)
{
    {
        QMutexLocker locker(&proxyMutex());
        proxyState() = setting;
    }
    proxyVersion().ref();
}

ProxySetting globalProxy()
{
    QMutexLocker locker(&proxyMutex());
    return proxyState();
}

bool parseProxyMode(const QString &s, ProxyMode *out)
{
    if (s == QLatin1String("off")) { *out = ProxyMode::Off; return true; }
    if (s == QLatin1String("auto")) { *out = ProxyMode::Auto; return true; }
    if (s == QLatin1String("manual")) { *out = ProxyMode::Manual; return true; }
    return false;
}

QString proxyModeToString(ProxyMode mode)
{
    switch (mode) {
    case ProxyMode::Off: return QStringLiteral("off");
    case ProxyMode::Auto: return QStringLiteral("auto");
    case ProxyMode::Manual: return QStringLiteral("manual");
    }
    return QStringLiteral("auto");
}

QByteArray HttpResult::header(const QByteArray &name) const
{
    return headers.value(name.toLower());
}

BlockingClient::BlockingClient(int timeoutMs)
    : m_timeoutMs(timeoutMs)
{
}

HttpResult BlockingClient::request(const QByteArray &method, const QUrl &url,
                                   const QMap<QByteArray, QByteArray> &headers,
                                   const QByteArray &body,
                                   const QByteArray &contentType,
                                   bool followRedirects)
{
    QNetworkRequest req(url);
    req.setTransferTimeout(std::chrono::milliseconds(m_timeoutMs));
    for (auto it = headers.begin(); it != headers.end(); ++it)
        req.setRawHeader(it.key(), it.value());
    if (!contentType.isEmpty())
        req.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     followRedirects ? QNetworkRequest::NoLessSafeRedirectPolicy
                                     : QNetworkRequest::ManualRedirectPolicy);
    // 不手写 Accept-Encoding：让 QNAM 自行声明 gzip/deflate 并透明解压，
    // 与 Go http.Transport 的自动解压行为一致。

    QNetworkAccessManager *nam = sharedNam();
    QNetworkReply *reply = nam->sendCustomRequest(req, method, body);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(m_timeoutMs, reply, [reply]() {
        if (reply->isRunning())
            reply->abort();
    });
    loop.exec();

    HttpResult out;
    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (statusVar.isValid())
        out.status = statusVar.toInt();

    for (const auto &pair : reply->rawHeaderPairs())
        out.headers.insert(pair.first.toLower(), pair.second);

    const QVariant cookiesVar = reply->header(QNetworkRequest::SetCookieHeader);
    if (cookiesVar.isValid())
        out.cookies = cookiesVar.value<QList<QNetworkCookie>>();

    out.body = reply->readAll();

    if (out.status == 0)
        out.networkError = reply->errorString();

    reply->deleteLater();
    return out;
}

bool downloadToFile(const QString &url, const QString &destination,
                    const std::function<void(qint64, qint64)> &progress,
                    QString *err)
{
    QDir().mkpath(QFileInfo(destination).absolutePath());
    QFile out(destination);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err) *err = QStringLiteral("create file: %1").arg(out.errorString());
        return false;
    }

    QNetworkRequest req((QUrl(url)));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = sharedNam()->get(req);

    QEventLoop loop;
    qint64 total = -1;
    QObject::connect(reply, &QNetworkReply::downloadProgress,
                     [&](qint64 got, qint64 tot) {
                         total = tot;
                         if (progress)
                             progress(got, tot);
                     });
    QObject::connect(reply, &QIODevice::readyRead, [&]() {
        const QByteArray chunk = reply->readAll();
        if (out.write(chunk) != chunk.size() && err && err->isEmpty())
            *err = QStringLiteral("write file failed");
    });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    const int status = statusVar.isValid() ? statusVar.toInt() : 0;
    const auto netErr = reply->error();
    const QString errString = reply->errorString();
    reply->deleteLater();
    out.flush();
    out.close();

    if (netErr != QNetworkReply::NoError && (status < 200 || status >= 300)) {
        if (err) *err = QStringLiteral("download file: %1").arg(errString);
        QFile::remove(destination);
        return false;
    }
    if (status < 200 || status >= 300) {
        if (err) *err = QStringLiteral("bad status: %1").arg(status);
        QFile::remove(destination);
        return false;
    }
    if (!err || err->isEmpty())
        return true;
    QFile::remove(destination);
    return false;
}

HttpResult httpGetFull(const QUrl &url)
{
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = sharedNam()->get(req);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    HttpResult out;
    const QVariant statusVar = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (statusVar.isValid())
        out.status = statusVar.toInt();
    for (const auto &pair : reply->rawHeaderPairs())
        out.headers.insert(pair.first.toLower(), pair.second);
    out.body = reply->readAll();
    if (out.status == 0)
        out.networkError = reply->errorString();
    reply->deleteLater();
    return out;
}
