#pragma once

#include "models.h"
#include "beatport.h"

#include <QAtomicInt>
#include <QString>
#include <QMutex>
#include <functional>
#include <thread>

struct SchedulerOptions {
    int intervalSeconds = 3600;
    QString genre;
    int perPage = 50;
    QString dateFrom;
    QString dateTo;
};

// 定时扫描最新曲目：启动立即 scan 一次，之后每个 interval 扫描，直到 stop。
// scan panic 用 C++ 对应：所有异常在 logger 中记 "recovered from panic"。
class Scheduler {
public:
    using Logger = std::function<void(const QString &)>;
    using ResultsHandler = std::function<void(const QList<Track> &, QString *err)>;

    Scheduler(QSharedPointer<Beatport> bp, SchedulerOptions options,
              Logger logger, ResultsHandler handler);
    ~Scheduler();

    void start();
    void stop();
    bool running() const;

private:
    void scan();
    void run();

    QSharedPointer<Beatport> m_bp;
    SchedulerOptions m_options;
    Logger m_logger;
    ResultsHandler m_handler;

    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable QMutex m_runningMu;
    bool m_running = false;
};
