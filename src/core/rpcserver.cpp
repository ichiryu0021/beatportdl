// 内嵌 HTTP/JSON-RPC 传输层，逐点对齐 BPDL/internal/server/server.go：
//   POST /rpc     JSON-RPC 2.0（17 个方法，业务在 RpcCore）
//   GET  /events  Server-Sent Events（开场 ": connected"，data 帧，队列 16 满则丢弃）
//   GET  /health  200 "ok"
//
// 线程模型：QTcpServer 与所有 socket 活在专用 m_thread；请求解析在该线程完成，
// 阻塞业务丢到 detached std::thread 调 RpcCore::invoke，响应通过 QueuedConnection
// 回 socket 所在线程写出并关闭连接（Content-Length + Connection: close）。

#include "rpcserver.h"
#include "rpc_core.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QMutexLocker>
#include <QPointer>
#include <QCoreApplication>

#include <deque>
#include <thread>

namespace {

struct SocketStateImpl {
    QByteArray buffer;
    bool headersComplete = false;
    bool handled = false;
    bool isEventStream = false;
    int headerBytes = 0;
    int contentLength = 0;
    QByteArray methodLine;
};

// 完整 HTTP 响应（调用点必须在 socket 所在线程）。
void sendHttp(QTcpSocket *socket, int status, const QByteArray &statusText,
              const QByteArray &contentType, const QByteArray &body, bool close,
              const QByteArray &extraHeaders = {})
{
    QByteArray resp;
    resp.reserve(body.size() + 256);
    resp += "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText + "\r\n";
    resp += "Content-Type: " + contentType + "\r\n";
    resp += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    if (!extraHeaders.isEmpty())
        resp += extraHeaders;
    resp += close ? "Connection: close\r\n" : "Connection: keep-alive\r\n";
    resp += "\r\n";
    resp += body;
    socket->write(resp);
    socket->flush();
}

} // namespace

// SSE 单连接（与 rpcserver.h 的前置声明/友元同名，定义落在 cpp）：
// 只在 m_thread drain/write；offer 可从任意线程调用，
// 内部队列容量 16，满则丢弃（对齐 Go broker 的 non-blocking send）。
class SseConnection {
public:
    explicit SseConnection(QTcpSocket *s) : socket(s) {}

    void offer(const QByteArray &data)
    {
        {
            QMutexLocker lock(&mu);
            if (queue.size() >= 16)
                return; // 慢消费者：丢事件
            queue.push_back(data);
        }
        // 投递到 socket 所在线程（m_thread）执行 drain。
        QMetaObject::invokeMethod(socket, [this] { drain(); },
                                  Qt::QueuedConnection);
    }

    void drain()
    {
        QMutexLocker lock(&mu);
        while (!queue.empty()) {
            socket->write("data: " + queue.front() + "\n\n");
            queue.pop_front();
        }
        socket->flush();
    }

    QTcpSocket *socket;

private:
    QMutex mu;
    std::deque<QByteArray> queue;
};

// RpcServer::SocketState 在 cpp 内给出定义。
struct RpcServer::SocketState : SocketStateImpl {};

RpcServer::RpcServer(RpcCore *core, QObject *parent)
    : QObject(parent), m_core(core)
{
    // 业务核心事件 -> SSE 广播。本对象随后 moveToThread(m_thread)，
    // AutoConnection 在 emit 时按接收者当时所在线程解析为 queued。
    connect(m_core, &RpcCore::eventBroadcast, this, &RpcServer::broadcastToSse);
}

RpcServer::~RpcServer()
{
    shutdown();
    qDeleteAll(m_sseClients);
    qDeleteAll(m_sseClosed);
}

// --- 线程 / 监听 -------------------------------------------------------------

bool RpcServer::start(const QString &host, quint16 port, QString *err)
{
    m_thread = new QThread();
    m_thread->setObjectName(QStringLiteral("bp-rpc"));
    moveToThread(m_thread);
    m_thread->start();

    bool ok = false;
    QMetaObject::invokeMethod(this, "doListen", Qt::BlockingQueuedConnection,
                              Q_RETURN_ARG(bool, ok),
                              Q_ARG(QString, host), Q_ARG(quint16, port));
    if (!ok) {
        m_thread->quit();
        m_thread->wait();
        delete m_thread;
        m_thread = nullptr;
        if (QCoreApplication::instance())
            moveToThread(QCoreApplication::instance()->thread());
        if (err) {
            *err = QStringLiteral("cannot listen on %1:%2: %3 "
                                  "(another BPDownloader instance is already running)")
                       .arg(host)
                       .arg(port)
                       .arg(m_listenError);
        }
        return false;
    }

    m_address = QStringLiteral("%1:%2").arg(host).arg(port);
    return true;
}

bool RpcServer::doListen(const QString &host, quint16 port)
{
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &RpcServer::onNewConnection,
            Qt::DirectConnection);
    if (!m_server->listen(QHostAddress(host), port)) {
        m_listenError = m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    return true;
}

void RpcServer::shutdown()
{
    if (!m_thread)
        return;

    // 停调度器 + 等待在途业务（与旧实现顺序一致，逻辑在 RpcCore）。
    m_core->shutdown();

    if (m_thread->isRunning()) {
        // 在网络线程关闭监听并中断现存连接（SSE/RPC）。
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (m_server)
                    m_server->close();
                const auto sockets = m_socketStates.keys();
                for (QTcpSocket *s : sockets)
                    s->abort();
            },
            Qt::BlockingQueuedConnection);
        m_thread->quit();
        m_thread->wait(5000);
    }
    delete m_thread;
    m_thread = nullptr;

    if (QCoreApplication::instance() && thread() != QCoreApplication::instance()->thread())
        moveToThread(QCoreApplication::instance()->thread());
}

// --- 接入连接 ---------------------------------------------------------------

void RpcServer::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            continue;
        m_socketStates.insert(socket, new SocketState);

        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] {
                    SocketState *st = m_socketStates.value(socket, nullptr);
                    if (!st)
                        return;
                    st->buffer += socket->readAll();

                    while (true) {
                        if (!st->headersComplete) {
                            const int idx = st->buffer.indexOf("\r\n\r\n");
                            if (idx < 0)
                                return;
                            const QList<QByteArray> headerLines = st->buffer.left(idx).split('\n');
                            if (headerLines.isEmpty())
                                return;
                            st->methodLine = headerLines.first().trimmed();
                            for (int i = 1; i < headerLines.size(); ++i) {
                                const QByteArray line = headerLines.at(i).trimmed();
                                const int colon = line.indexOf(':');
                                if (colon <= 0)
                                    continue;
                                const QByteArray name = line.left(colon).trimmed().toLower();
                                const QByteArray value = line.mid(colon + 1).trimmed();
                                if (name == "content-length")
                                    st->contentLength = value.toInt();
                            }
                            st->headerBytes = idx + 4;
                            st->headersComplete = true;
                        }
                        if (st->buffer.size() < st->headerBytes + st->contentLength)
                            return;
                        if (st->handled)
                            return;
                        st->handled = true;

                        const QByteArray body =
                            st->buffer.mid(st->headerBytes, st->contentLength);
                        handleRequest(socket, body, st->methodLine);
                        return; // 每条连接只处理一个请求（SSE 除外，已转流式）
                    }
                },
            Qt::DirectConnection);

        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket] {
                    SocketState *st = m_socketStates.take(socket);
                    delete st;
                    socket->deleteLater();
                },
            Qt::DirectConnection);
    }
}

void RpcServer::handleRequest(QTcpSocket *socket, const QByteArray &body,
                              const QByteArray &methodLine)
{
    const QList<QByteArray> parts = methodLine.split(' ');
    if (parts.size() < 2) {
        sendHttp(socket, 400, "Bad Request", "text/plain; charset=utf-8",
                 "bad request\n", true);
        socket->disconnectFromHost();
        return;
    }
    const QByteArray httpMethod = parts.at(0);
    const QByteArray target = parts.at(1);
    const QByteArray path = target.contains('?') ? target.left(target.indexOf('?'))
                                                 : target;

    if (path == "/health") {
        handleHealth(socket);
        return;
    }
    if (path == "/events") {
        handleEvents(socket);
        return;
    }
    if (path == "/rpc") {
        if (httpMethod != "POST") {
            // Go net/http: http.Error(w, "method not allowed", 405)
            sendHttp(socket, 405, "Method Not Allowed", "text/plain; charset=utf-8",
                     "method not allowed\n", true,
                     "X-Content-Type-Options: nosniff\r\n");
            socket->disconnectFromHost();
            return;
        }
        handleRpc(socket, body, QJsonValue());
        return;
    }

    sendHttp(socket, 404, "Not Found", "text/plain; charset=utf-8",
             "404 page not found\n", true,
             "X-Content-Type-Options: nosniff\r\n");
    socket->disconnectFromHost();
}

void RpcServer::handleHealth(QTcpSocket *socket)
{
    sendHttp(socket, 200, "OK", "text/plain; charset=utf-8", "ok", true);
    socket->disconnectFromHost();
}

void RpcServer::handleEvents(QTcpSocket *socket)
{
    SocketState *st = m_socketStates.value(socket, nullptr);
    if (st)
        st->isEventStream = true;

    QByteArray head;
    head += "HTTP/1.1 200 OK\r\n";
    head += "Content-Type: text/event-stream\r\n";
    head += "Cache-Control: no-cache\r\n";
    head += "Connection: keep-alive\r\n";
    head += "\r\n";
    head += ": connected\n\n";
    socket->write(head);
    socket->flush();

    auto *client = new SseConnection(socket);
    {
        QMutexLocker lock(&m_sseMu);
        m_sseClients.insert(client);
    }
    connect(socket, &QTcpSocket::disconnected, this,
            [this, client] { removeSseClient(client); }, Qt::DirectConnection);
}

void RpcServer::removeSseClient(SseConnection *client)
{
    QMutexLocker lock(&m_sseMu);
    m_sseClients.remove(client);
    m_sseClosed.insert(client); // 延迟到网络线程停止后删除，防排队事件悬空
}

void RpcServer::broadcastToSse(const QJsonObject &event)
{
    const QByteArray data = QJsonDocument(event).toJson(QJsonDocument::Compact);
    QMutexLocker lock(&m_sseMu);
    for (SseConnection *c : std::as_const(m_sseClients))
        c->offer(data);
}

// --- JSON-RPC ---------------------------------------------------------------

void RpcServer::handleRpc(QTcpSocket *socket, const QByteArray &body,
                          const QJsonValue &fallbackId)
{
    QJsonParseError parseErr;
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseErr);
    if (parseErr.error != QJsonParseError::NoError || !doc.isObject()) {
        // 解析失败：无 id（序列化为 null）。
        QJsonObject resp;
        QJsonObject e;
        e["code"] = -32700;
        e["message"] = QStringLiteral("parse error: ") + parseErr.errorString();
        resp["error"] = e;
        resp["id"] = QJsonValue();
        const QByteArray payload = QJsonDocument(resp).toJson(QJsonDocument::Compact);
        sendHttp(socket, 200, "OK", "application/json", payload, true);
        socket->disconnectFromHost();
        return;
    }

    const QJsonObject obj = doc.object();
    const QJsonValue methodVal = obj.value(QStringLiteral("method"));
    if (!methodVal.isString()) {
        // Go: method 类型不对在 Decode 阶段失败 → -32700。
        QJsonObject resp;
        QJsonObject e;
        e["code"] = -32700;
        e["message"] = QStringLiteral("parse error: method must be a string");
        resp["error"] = e;
        resp["id"] = QJsonValue();
        const QByteArray payload = QJsonDocument(resp).toJson(QJsonDocument::Compact);
        sendHttp(socket, 200, "OK", "application/json", payload, true);
        socket->disconnectFromHost();
        return;
    }

    QJsonObject params;
    const QJsonValue paramsVal = obj.value(QStringLiteral("params"));
    if (!paramsVal.isUndefined() && !paramsVal.isNull()) {
        if (!paramsVal.isObject()) {
            QJsonObject resp;
            QJsonObject e;
            e["code"] = -32602;
            e["message"] = QStringLiteral("invalid params: params must be a JSON object");
            resp["error"] = e;
            resp["id"] = obj.value(QStringLiteral("id"));
            const QByteArray payload = QJsonDocument(resp).toJson(QJsonDocument::Compact);
            sendHttp(socket, 200, "OK", "application/json", payload, true);
            socket->disconnectFromHost();
            return;
        }
        params = paramsVal.toObject();
    }

    QJsonValue idVal = obj.value(QStringLiteral("id"));
    if (idVal.isUndefined())
        idVal = fallbackId.isUndefined() ? QJsonValue() : fallbackId;

    const QString method = methodVal.toString();

    // 阻塞业务在独立线程执行 RpcCore；响应排队回 socket 所在线程写出。
    QPointer<QTcpSocket> guarded(socket);
    std::thread([this, guarded, method, params, idVal]() mutable {
        QJsonObject resp = m_core->invoke(method, params);
        resp["id"] = idVal.isUndefined() ? QJsonValue() : idVal;
        const QByteArray payload = QJsonDocument(resp).toJson(QJsonDocument::Compact);

        if (QTcpSocket *s = guarded.data()) {
            QMetaObject::invokeMethod(
                s,
                [s, payload] {
                    sendHttp(s, 200, "OK", "application/json", payload, true);
                    s->disconnectFromHost();
                },
                Qt::QueuedConnection);
        }
    }).detach();
}
