#include "scheduler.h"

#include <QMutexLocker>
#include <chrono>
#include <exception>
#include <thread>

Scheduler::Scheduler(QSharedPointer<Beatport> bp, SchedulerOptions options,
                     Logger logger, ResultsHandler handler)
    : m_bp(std::move(bp))
    , m_options(std::move(options))
    , m_logger(std::move(logger))
    , m_handler(std::move(handler))
{
}

Scheduler::~Scheduler()
{
    stop();
}

void Scheduler::start()
{
    QMutexLocker lock(&m_runningMu);
    if (m_running)
        return;
    m_stop.store(false);
    m_running = true;
    m_thread = std::thread(&Scheduler::run, this);
}

void Scheduler::stop()
{
    m_stop.store(true);
    QMutexLocker lock(&m_runningMu);
    if (m_thread.joinable())
        m_thread.join();
    m_running = false;
}

bool Scheduler::running() const
{
    QMutexLocker lock(&m_runningMu);
    return m_running;
}

void Scheduler::run()
{
    // Go：for { scan(); select { case <-time.After(interval): case <-stop: } }
    // 100ms 分片等待，stop 后最迟 100ms 退出。
    const int totalMs = m_options.intervalSeconds * 1000;
    constexpr int kStepMs = 100;
    while (!m_stop.load()) {
        scan();
        for (int waited = 0;
             waited < totalMs && !m_stop.load();
             waited += kStepMs) {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(kStepMs));
        }
    }
}

void Scheduler::scan()
{
    // 对应 Go scan() 的 defer recover()：任何异常都收敛为日志，不炸线程。
    try {
        if (m_logger)
            m_logger(QStringLiteral("scheduler: starting scan"));

        QString fetchErr;
        const QSharedPointer<PaginatedTracks> res = m_bp->getLatestTracks(
            m_options.genre, 1, m_options.perPage,
            m_options.dateFrom, m_options.dateTo, &fetchErr);
        if (!res) {
            if (m_logger)
                m_logger(QStringLiteral("scheduler: fetch failed: %1")
                             .arg(fetchErr));
            return;
        }

        const QList<Track> tracks = res->results;
        if (m_logger) {
            m_logger(QStringLiteral("scheduler: scan returned %1 tracks")
                         .arg(tracks.size()));
        }

        if (m_handler) {
            QString handlerErr;
            m_handler(tracks, &handlerErr);
            if (!handlerErr.isEmpty() && m_logger) {
                m_logger(QStringLiteral("scheduler: result handler error: %1")
                             .arg(handlerErr));
            }
        }
    } catch (const std::exception &e) {
        if (m_logger) {
            m_logger(QStringLiteral("scheduler: recovered from panic: %1")
                         .arg(QString::fromUtf8(e.what())));
        }
    } catch (...) {
        if (m_logger) {
            m_logger(QStringLiteral(
                "scheduler: recovered from panic: <unknown C++ exception>"));
        }
    }
}
