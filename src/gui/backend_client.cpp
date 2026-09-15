#include "backend_client.h"
#include "core/rpc_core.h"

#include <QNetworkRequest>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUrl>
#include <QIODevice>
#include <QTimer>
#include <QCoreApplication>

#include <thread>

BackendClient::BackendClient(QObject *parent)
    : QObject(parent), m_nam(this)
{
}

void BackendClient::attachLocal(RpcCore *core)
{
    m_local = core;
    m_reachable = true; // 同进程：始终可达
}

void BackendClient::rpc(const QString &method,
                        const QJsonObject &params,
                        std::function<void(QJsonValue)> onSuccess,
                        std::function<void(QString)> onError)
{
    if (m_local) {
        // 进程内直连：阻塞业务放工作线程，结果排队回本对象所在的 GUI 线程。
        // 本对象销毁后排队事件连同回调一并被 Qt 摘除，不会触碰已析构界面。
        RpcCore *core = m_local;
        QPointer<BackendClient> self(this);
        std::thread([self, core, method, params, onSuccess, onError]() mutable {
            const QJsonObject resp = core->invoke(method, params);
            if (!self)
                return;
            QMetaObject::invokeMethod(
                self.data(),
                [self, resp, onSuccess, onError]() {
                    if (!self)
                        return;
                    if (resp.contains(QStringLiteral("error"))
                        && !resp.value(QStringLiteral("error")).isNull()) {
                        const QJsonObject err =
                            resp.value(QStringLiteral("error")).toObject();
                        QString msg = err.value(QStringLiteral("message")).toString();
                        if (msg.isEmpty())
                            msg = QString::fromUtf8(
                                QJsonDocument(err).toJson(QJsonDocument::Compact));
                        if (onError) onError(msg);
                        return;
                    }
                    if (onSuccess) onSuccess(resp.value(QStringLiteral("result")));
                },
                Qt::QueuedConnection);
        }).detach();
        return;
    }

    const int id = m_nextId++;
    QJsonObject req;
    req.insert("jsonrpc", QStringLiteral("2.0"));
    req.insert("method", method);
    req.insert("params", params);
    req.insert("id", id);

    QNetworkRequest request(QUrl(QString::fromLatin1(baseUrl()) + QStringLiteral("/rpc")));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *reply = m_nam.post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));

    QObject::connect(reply, &QNetworkReply::finished, this, [reply, onSuccess, onError]() {
        QScopedPointer<QNetworkReply, QScopedPointerDeleteLater> guard(reply);
        if (reply->error() != QNetworkReply::NoError) {
            if (onError) onError(reply->errorString());
            return;
        }
        QJsonParseError parseErr;
        QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &parseErr);
        if (parseErr.error != QJsonParseError::NoError) {
            if (onError) onError(tr("The backend returned invalid JSON: ") + parseErr.errorString());
            return;
        }
        if (!doc.isObject()) {
            if (onError) onError(tr("The backend response is not a JSON object"));
            return;
        }
        const QJsonObject obj = doc.object();
        if (obj.contains("error") && !obj.value("error").isNull()) {
            const QJsonObject err = obj.value("error").toObject();
            QString msg = err.value("message").toString();
            if (msg.isEmpty())
                msg = QString::fromUtf8(QJsonDocument(err).toJson(QJsonDocument::Compact));
            if (onError) onError(msg);
            return;
        }
        if (onSuccess) onSuccess(obj.value("result"));
    });
}

QNetworkReply *BackendClient::subscribeEvents(std::function<void(QJsonObject)> onEvent)
{
    if (m_local) {
        // 直连：业务核心信号 -> queued -> GUI 线程回调；重订先断开旧连接。
        QObject::disconnect(m_localEventsConn);
        m_localEventsConn = QObject::connect(
            m_local, &RpcCore::eventBroadcast, this,
            [onEvent](const QJsonObject &ev) { if (onEvent) onEvent(ev); },
            Qt::QueuedConnection);
        return nullptr;
    }

    if (m_eventsReply) {
        m_eventsReply->abort();
        m_eventsReply->deleteLater();
        m_eventsReply = nullptr;
    }

    QNetworkRequest request(QUrl(QString::fromLatin1(baseUrl()) + QStringLiteral("/events")));
    request.setRawHeader("Accept", "text/event-stream");
    QNetworkReply *reply = m_nam.get(request);
    m_eventsReply = reply;
    m_eventsBuffer.clear();

    QObject::connect(reply, &QIODevice::readyRead, this, [this, reply, onEvent]() {
        if (m_eventsReply != reply) return; // stale reply after re-subscribe
        m_eventsBuffer.append(reply->readAll());
        m_eventsBuffer.replace("\r", ""); // tolerate CRLF line endings
        int idx;
        while ((idx = m_eventsBuffer.indexOf("\n\n")) >= 0) {
            const QByteArray chunk = m_eventsBuffer.left(idx);
            m_eventsBuffer.remove(0, idx + 2);
            for (const QByteArray &rawLine : chunk.split('\n')) {
                QByteArray line = rawLine.trimmed();
                if (!line.startsWith("data:")) continue;
                QByteArray payload = line.mid(5).trimmed();
                if (payload.isEmpty()) continue;
                QJsonParseError pe;
                QJsonDocument doc = QJsonDocument::fromJson(payload, &pe);
                if (pe.error != QJsonParseError::NoError || !doc.isObject()) continue;
                if (onEvent) onEvent(doc.object());
            }
        }
    });

    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        if (m_eventsReply == reply) m_eventsReply = nullptr;
        reply->deleteLater();
    });

    return reply;
}

bool BackendClient::isReachable() const
{
    return m_local ? true : m_reachable;
}

void BackendClient::checkHealth(std::function<void(bool)> cb)
{
    if (m_local) {
        // 同进程直连恒可达；仍异步回调，保持与 HTTP 路径一致的时序。
        m_reachable = true;
        QTimer::singleShot(0, this, [cb] { if (cb) cb(true); });
        return;
    }

    QNetworkRequest request(QUrl(QString::fromLatin1(baseUrl()) + QStringLiteral("/health")));
    QNetworkReply *reply = m_nam.get(request);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, cb]() {
        const bool ok = (reply->error() == QNetworkReply::NoError);
        m_reachable = ok;
        reply->deleteLater();
        if (cb) cb(ok);
    });
}

void BackendClient::authStatus(std::function<void(bool, QString)> cb)
{
    rpc(QStringLiteral("authStatus"), QJsonObject{},
        [cb](QJsonValue result) {
            bool ready = false;
            QString err;
            if (result.isObject()) {
                const QJsonObject obj = result.toObject();
                ready = obj.value(QStringLiteral("ready")).toBool(false);
                err = obj.value(QStringLiteral("error")).toString();
            }
            if (cb) cb(ready, err);
        },
        [cb](QString err) { if (cb) cb(false, err); });
}

void BackendClient::login(const QString &username, const QString &password,
                          std::function<void()> onSuccess,
                          std::function<void(QString)> onError)
{
    QJsonObject params;
    params.insert(QStringLiteral("username"), username);
    params.insert(QStringLiteral("password"), password);
    rpc(QStringLiteral("login"), params,
        [onSuccess](QJsonValue) { if (onSuccess) onSuccess(); },
        [onError](QString err) { if (onError) onError(err); });
}

void BackendClient::logout(std::function<void()> onSuccess,
                           std::function<void(QString)> onError)
{
    rpc(QStringLiteral("logout"), QJsonObject{},
        [onSuccess](QJsonValue) { if (onSuccess) onSuccess(); },
        [onError](QString err) { if (onError) onError(err); });
}
