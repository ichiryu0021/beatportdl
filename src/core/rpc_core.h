#pragma once

#include "downloader.h"
#include "scheduler.h"

#include <QObject>
#include <QJsonObject>
#include <QJsonValue>
#include <QMutex>
#include <QWaitCondition>
#include <memory>

// RpcCore：与传输无关的 JSON-RPC 业务核心（对齐 Go internal/server 的方法集）。
// 两条前端共用同一套实现：
//   - --serve：RpcServer 经 HTTP/SSE 调用 invoke()/订阅 eventBroadcast；
//   - GUI onefile：BackendClient 直连本对象（进程内调用 + Qt 信号），不绑端口。
//
// 线程模型：invoke() 可从任意线程调用（阻塞式执行，调用方自行放在工作线程）；
// publishEvent() 由下载/调度线程触发，以 Qt 信号向各订阅者跨线程投递。
class RpcCore : public QObject, public EventSink {
    Q_OBJECT
public:
    explicit RpcCore(Application *app, QObject *parent = nullptr);
    ~RpcCore() override;

    // 任意线程调用。返回 JSON-RPC 信封：{"result":...} 或 {"error":{code,message}}。
    // 内部统计在途调用数，shutdown() 可等待其结束，避免 Application 析构后触碰。
    QJsonObject invoke(const QString &method, const QJsonObject &params);

    // 停调度器 + 等待在途业务线程（最多 3s）。GUI/--serve 退出前调用。
    void shutdown();

    // EventSink：任意线程。
    void publishEvent(const QJsonObject &event) override;

signals:
    // 任意线程 emit；跨线程订阅者经 QueuedConnection 在自身线程收到。
    void eventBroadcast(const QJsonObject &event);

private:
    QJsonObject dispatch(const QString &method, const QJsonObject &p, bool *hadError);

    // 各 RPC 方法（参数/返回与 internal/server/rpc*.go 严格一致）
    QJsonValue rpcSearch(const QJsonObject &p);
    QJsonValue rpcGetLatest(const QJsonObject &p);
    QJsonValue rpcGetGenres(const QJsonObject &p);
    QJsonValue rpcDownload(const QJsonObject &p);
    QJsonValue rpcSetDownloadsDirectory(const QJsonObject &p);
    QJsonValue rpcGetDownloadsDirectory(const QJsonObject &p);
    QJsonValue rpcGetSettings(const QJsonObject &p);
    QJsonValue rpcSetQuality(const QJsonObject &p);
    QJsonValue rpcSetTrackExists(const QJsonObject &p);
    QJsonValue rpcSetTrackFileTemplate(const QJsonObject &p);
    QJsonValue rpcSetProxy(const QJsonObject &p);
    QJsonValue rpcSetTheme(const QJsonObject &p);
    QJsonValue rpcSetLanguage(const QJsonObject &p);
    QJsonValue rpcSetGenreSorting(const QJsonObject &p);
    QJsonValue rpcLogin(const QJsonObject &p);
    QJsonValue rpcLogout(const QJsonObject &p);
    QJsonValue rpcAuthStatus(const QJsonObject &p);
    QJsonValue rpcStartScheduler(const QJsonObject &p);
    QJsonValue rpcStopScheduler(const QJsonObject &p);
    QJsonValue rpcSchedulerStatus(const QJsonObject &p);

    Application *m_app;

    QMutex m_schedulerMu;
    std::unique_ptr<Scheduler> m_scheduler;

    QMutex m_inFlightMu;
    QWaitCondition m_inFlightCv;
    int m_inFlight = 0;
};
