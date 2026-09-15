#pragma once

#include <QObject>
#include <QHash>
#include <QMutex>
#include <QSet>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QThread;
class RpcCore;
class SseConnection;
class QJsonObject;
class QJsonValue;

// 内嵌 HTTP 传输层（对应 internal/server.Server 的 HTTP 面）：
//   POST /rpc     JSON-RPC 2.0（业务全部委托 RpcCore）
//   GET  /events  SSE（订阅 RpcCore::eventBroadcast）
//   GET  /health  200 "ok"
// 绑定地址固定回环（默认 127.0.0.1:8765）。仅 --serve 模式使用；
// GUI onefile 直连 RpcCore，不创建本对象、不监听任何端口。
class RpcServer : public QObject {
    Q_OBJECT
public:
    explicit RpcServer(RpcCore *core, QObject *parent = nullptr);
    ~RpcServer() override;

    // 在内部网络线程中绑定监听；失败返回 false（err 含
    // "cannot listen on 127.0.0.1:8765: ... (another BPDownloader instance is already running)"）。
    bool start(const QString &host, quint16 port, QString *err);
    void shutdown();
    QString address() const { return m_address; }

private slots:
    bool doListen(const QString &host, quint16 port);
    void onNewConnection();

private:
    void handleRequest(QTcpSocket *socket, const QByteArray &rawRequest,
                       const QByteArray &methodLine);
    void handleRpc(QTcpSocket *socket, const QByteArray &body, const QJsonValue &id);
    void handleHealth(QTcpSocket *socket);
    void handleEvents(QTcpSocket *socket);
    void removeSseClient(SseConnection *client);
    void broadcastToSse(const QJsonObject &event);

    RpcCore *m_core;
    QTcpServer *m_server = nullptr;
    QThread *m_thread = nullptr;
    QString m_address;
    QString m_listenError;

    // 仅在网络线程（m_thread）访问：socket 的收包缓冲/解析状态。
    struct SocketState;
    QHash<QTcpSocket *, SocketState *> m_socketStates;

    QMutex m_sseMu;
    QSet<SseConnection *> m_sseClients;
    QSet<SseConnection *> m_sseClosed; // 已断开但可能仍有排队 drain 事件，线程停止后统一删除

    friend class SseConnection;
};
