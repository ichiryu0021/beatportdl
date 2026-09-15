#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QByteArray>
#include <functional>

class RpcCore;

// GUI 与下载核心之间的客户端，两种传输：
//   1) onefile 直连（attachLocal 后）：进程内工作线程调 RpcCore::invoke，
//      事件走 Qt 信号——不监听任何端口；
//   2) HTTP 回环（默认）：JSON-RPC 2.0 over POST /rpc + SSE over GET /events，
//      供独立 --serve 后端场景。
// 所有回调都在 GUI 线程触发。
class BackendClient : public QObject
{
    Q_OBJECT
public:
    explicit BackendClient(QObject *parent = nullptr);

    // 切换为进程内直连模式（必须在任何 rpc()/subscribeEvents() 之前调用）。
    void attachLocal(RpcCore *core);

    // Send a JSON-RPC 2.0 request to POST /rpc. Parses {result,error,id}
    // and routes to onSuccess (QJsonValue of "result") or onError.
    void rpc(const QString &method,
             const QJsonObject &params,
             std::function<void(QJsonValue)> onSuccess,
             std::function<void(QString)> onError);

    // Subscribe to the SSE stream at GET /events. Each parsed JSON event
    // object is delivered to onEvent. The returned reply must stay alive
    // for the lifetime of the subscription; this client keeps it alive
    // internally (m_eventsReply) so callers may ignore the return value.
    // 直连模式下返回 nullptr（订阅以 Qt 连接形式保存在客户端内部）。
    QNetworkReply *subscribeEvents(std::function<void(QJsonObject)> onEvent);

    // True if the last /health check reached the back-end.
    bool isReachable() const;

    // GET /health; updates isReachable() and invokes cb with the result.
    void checkHealth(std::function<void(bool)> cb);

    // POST /rpc {"method":"authStatus"} → {ready:bool,error?:string}
    // 前端启动时先查认证状态，决定是否显示登录页。
    void authStatus(std::function<void(bool ready, QString err)> cb);

    // POST /rpc {"method":"login","params":{username,password}}
    // 用前端提交的凭据做 Beatport OAuth 登录。成功后后端写回配置+缓存。
    void login(const QString &username, const QString &password,
               std::function<void()> onSuccess,
               std::function<void(QString err)> onError);

    // POST /rpc {"method":"logout"} 清除后端凭据与缓存；之后应回到登录页。
    void logout(std::function<void()> onSuccess,
                std::function<void(QString err)> onError);

    static const char *baseUrl() { return "http://127.0.0.1:8765"; }

private:
    RpcCore *m_local = nullptr; // 非空 = 进程内直连模式
    QMetaObject::Connection m_localEventsConn;
    QNetworkAccessManager m_nam;
    QPointer<QNetworkReply> m_eventsReply; // SSE reply must stay alive
    bool m_reachable = false;
    int m_nextId = 1; // incrementing JSON-RPC id
    QByteArray m_eventsBuffer;
};
