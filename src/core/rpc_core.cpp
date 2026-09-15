// 与传输无关的 JSON-RPC 业务核心，实现逐点对齐 BPDL/internal/server/server.go
// 的 17 个 RPC 方法。HTTP/SSE 传输见 rpcserver.cpp，GUI 直连见 backend_client.cpp。

#include "rpc_core.h"

#include <QMutexLocker>
#include <QJsonArray>
#include <QSet>

namespace {

QString strParam(const QJsonObject &p, const char *key)
{
    return p.value(QLatin1String(key)).toString();
}

QJsonObject trackToMap(const Track &t)
{
    QJsonObject o;
    o["id"] = t.id;
    o["url"] = t.url;
    o["name"] = t.name;
    o["mix_name"] = t.mixName;
    QJsonArray artists;
    artists.append(QJsonObject{{QStringLiteral("name"), t.artists.display(0, QString())}});
    o["artists"] = artists;
    o["genre"] = QJsonObject{{QStringLiteral("name"), t.genre.name}};
    return o;
}

QJsonObject releaseToMap(const Release &r)
{
    QJsonObject o;
    o["id"] = r.id;
    o["url"] = r.url;
    o["name"] = r.name;
    QJsonArray artists;
    artists.append(QJsonObject{{QStringLiteral("name"), r.artists.display(0, QString())}});
    o["artists"] = artists;
    return o;
}

QJsonArray genreArray(const QList<Genre> &genres)
{
    QJsonArray arr;
    for (const Genre &g : genres) {
        QJsonObject o;
        o["id"] = g.id;
        o["name"] = g.name;
        o["slug"] = g.slug;
        arr.append(o);
    }
    return arr;
}

struct LatestFetch {
    QJsonArray results;
    int count = 0;
    bool hasMore = false;
};

} // namespace

RpcCore::RpcCore(Application *app, QObject *parent)
    : QObject(parent), m_app(app)
{
}

RpcCore::~RpcCore()
{
    shutdown();
}

// --- 生命周期 / 事件 ---------------------------------------------------------

void RpcCore::shutdown()
{
    // 先停调度器（join 其扫描线程，回调里还会用 m_app/publishEvent）。
    {
        QMutexLocker lock(&m_schedulerMu);
        m_scheduler.reset();
    }
    // 等待在途 RPC 业务线程一小段时间，避免它们在 Application 析构后继续访问。
    QMutexLocker lock(&m_inFlightMu);
    if (m_inFlight > 0)
        m_inFlightCv.wait(&m_inFlightMu, 3000);
}

void RpcCore::publishEvent(const QJsonObject &event)
{
    emit eventBroadcast(event);
}

QJsonObject RpcCore::invoke(const QString &method, const QJsonObject &params)
{
    {
        QMutexLocker lock(&m_inFlightMu);
        ++m_inFlight;
    }

    QJsonObject dispatched;
    bool hadError = false;
    try {
        dispatched = dispatch(method, params, &hadError);
    } catch (const RpcException &e) {
        hadError = true;
        dispatched = QJsonObject{{QStringLiteral("code"), e.code},
                                 {QStringLiteral("message"), e.message}};
    } catch (...) {
        hadError = true;
        dispatched = QJsonObject{{QStringLiteral("code"), -32000},
                                 {QStringLiteral("message"),
                                  QStringLiteral("internal server error")}};
    }

    QJsonObject resp;
    if (hadError)
        resp["error"] = dispatched;
    else
        resp["result"] = dispatched.value(QStringLiteral("result"));

    {
        QMutexLocker lock(&m_inFlightMu);
        if (--m_inFlight == 0)
            m_inFlightCv.wakeAll();
    }
    return resp;
}

QJsonObject RpcCore::dispatch(const QString &method, const QJsonObject &p, bool *hadError)
{
    *hadError = false;
    QJsonValue result;
    try {
        if (method == QLatin1String("search"))
            result = rpcSearch(p);
        else if (method == QLatin1String("getLatest"))
            result = rpcGetLatest(p);
        else if (method == QLatin1String("getGenres"))
            result = rpcGetGenres(p);
        else if (method == QLatin1String("download"))
            result = rpcDownload(p);
        else if (method == QLatin1String("setDownloadsDirectory"))
            result = rpcSetDownloadsDirectory(p);
        else if (method == QLatin1String("getDownloadsDirectory"))
            result = rpcGetDownloadsDirectory(p);
        else if (method == QLatin1String("getSettings"))
            result = rpcGetSettings(p);
        else if (method == QLatin1String("setQuality"))
            result = rpcSetQuality(p);
        else if (method == QLatin1String("setTrackExists"))
            result = rpcSetTrackExists(p);
        else if (method == QLatin1String("setTrackFileTemplate"))
            result = rpcSetTrackFileTemplate(p);
        else if (method == QLatin1String("setProxy"))
            result = rpcSetProxy(p);
        else if (method == QLatin1String("setTheme"))
            result = rpcSetTheme(p);
        else if (method == QLatin1String("setLanguage"))
            result = rpcSetLanguage(p);
        else if (method == QLatin1String("setGenreSorting"))
            result = rpcSetGenreSorting(p);
        else if (method == QLatin1String("login"))
            result = rpcLogin(p);
        else if (method == QLatin1String("logout"))
            result = rpcLogout(p);
        else if (method == QLatin1String("authStatus"))
            result = rpcAuthStatus(p);
        else if (method == QLatin1String("startScheduler"))
            result = rpcStartScheduler(p);
        else if (method == QLatin1String("stopScheduler"))
            result = rpcStopScheduler(p);
        else if (method == QLatin1String("schedulerStatus"))
            result = rpcSchedulerStatus(p);
        else
            throw RpcException{-32601, QStringLiteral("method not found: ") + method};
    } catch (const RpcException &e) {
        *hadError = true;
        return QJsonObject{{QStringLiteral("code"), e.code},
                           {QStringLiteral("message"), e.message}};
    }
    return QJsonObject{{QStringLiteral("result"), result}};
}

// --- 17 个 RPC 方法 ----------------------------------------------------------

QJsonValue RpcCore::rpcSearch(const QJsonObject &p)
{
    const QString query = strParam(p, "query");
    if (query.isEmpty())
        throw RpcException{-32602, QStringLiteral("query is required")};

    QString err;
    auto found = m_app->bp->search(query, &err);
    if (!found)
        throw RpcException{-32000, QStringLiteral("search failed: ") + err};

    QJsonArray out;
    for (const Track &t : found->tracks) {
        QJsonObject o;
        o["type"] = QStringLiteral("track");
        o["id"] = t.id;
        o["title"] = t.name + QStringLiteral(" (") + t.mixName + QLatin1Char(')');
        o["artists"] = t.artists.display(0, QString());
        o["url"] = t.url;
        if (!t.genre.name.isEmpty())
            o["genre"] = t.genre.name;
        out.append(o);
    }
    for (const Release &r : found->releases) {
        QJsonObject o;
        o["type"] = QStringLiteral("release");
        o["id"] = r.id;
        o["title"] = r.name;
        o["artists"] = r.artists.display(0, QString());
        o["url"] = r.url;
        out.append(o);
    }
    for (const Label &l : found->labels) {
        QJsonObject o;
        o["type"] = QStringLiteral("label");
        o["id"] = l.id;
        o["title"] = l.name;
        o["url"] = l.storeUrl();
        out.append(o);
    }
    return out;
}

QJsonValue RpcCore::rpcGetLatest(const QJsonObject &p)
{
    int page = p.value(QStringLiteral("page")).toInt(0);
    int perPage = p.value(QStringLiteral("per_page")).toInt(0);
    if (page <= 0)
        page = 1;
    if (perPage <= 0 || perPage > 100)
        perPage = 50;

    QStringList genres;
    const QJsonArray genresArr = p.value(QStringLiteral("genres")).toArray();
    for (const QJsonValue &v : genresArr) {
        const QString g = v.toString();
        if (!g.isEmpty())
            genres.append(g);
    }
    const QString singleGenre = strParam(p, "genre");
    if (genres.isEmpty() && !singleGenre.isEmpty())
        genres.append(singleGenre);

    const QString kind = strParam(p, "kind");
    const QString dateFrom = strParam(p, "date_from");
    const QString dateTo = strParam(p, "date_to");
    const bool releases = (kind == QLatin1String("releases"));

    auto fetch = [&](const QString &genre) -> LatestFetch {
        LatestFetch out;
        QString err;
        if (releases) {
            auto res = m_app->bp->getLatestReleases(genre, page, perPage, dateFrom,
                                                    dateTo, &err);
            if (!res)
                throw RpcException{-32000, err};
            out.count = res->count;
            out.hasMore = res->hasNext;
            for (const Release &r : res->results)
                out.results.append(releaseToMap(r));
        } else {
            auto res = m_app->bp->getLatestTracks(genre, page, perPage, dateFrom,
                                                  dateTo, &err);
            if (!res)
                throw RpcException{-32000, err};
            out.count = res->count;
            out.hasMore = res->hasNext;
            for (const Track &t : res->results)
                out.results.append(trackToMap(t));
        }
        return out;
    };

    auto buildResult = [page, perPage](const LatestFetch &f) {
        QJsonObject o;
        o["results"] = f.results;
        o["page"] = page;
        o["per_page"] = perPage;
        o["count"] = f.count;
        o["has_more"] = f.hasMore;
        return o;
    };

    if (genres.size() <= 1) {
        const QString genre = genres.isEmpty() ? QString() : genres.first();
        try {
            return buildResult(fetch(genre));
        } catch (const RpcException &e) {
            throw RpcException{-32000, QStringLiteral("getLatest failed: ") + e.message};
        }
    }

    // 多曲风：按结果 url 去重合并，count 累加，has_more 取或。
    LatestFetch merged;
    QSet<QString> seen;
    for (const QString &g : genres) {
        LatestFetch f;
        try {
            f = fetch(g);
        } catch (const RpcException &e) {
            throw RpcException{-32000,
                               QStringLiteral("getLatest failed for genre %1: %2")
                                   .arg(g, e.message)};
        }
        merged.count += f.count;
        merged.hasMore = merged.hasMore || f.hasMore;
        for (const QJsonValue &m : f.results) {
            const QString key = m.toObject().value(QStringLiteral("url")).toString();
            if (seen.contains(key))
                continue;
            seen.insert(key);
            merged.results.append(m);
        }
    }
    return buildResult(merged);
}

QJsonValue RpcCore::rpcGetGenres(const QJsonObject &)
{
    QString err;
    const QList<Genre> scraped = m_app->bp->scrapeGenres(&err);
    if (err.isEmpty() && !scraped.isEmpty())
        return genreArray(scraped);

    auto res = m_app->bp->getGenres(&err);
    if (!res)
        throw RpcException{-32000, QStringLiteral("getGenres failed: ") + err};
    return genreArray(res->results);
}

QJsonValue RpcCore::rpcLogin(const QJsonObject &p)
{
    const QString username = strParam(p, "username");
    const QString password = strParam(p, "password");
    if (username.isEmpty() || password.isEmpty())
        throw RpcException{-32602, QStringLiteral("username and password are required")};
    try {
        m_app->login(username, password);
    } catch (const RpcException &e) {
        // Application::login 的错误信息已自带上下文，不再重复加前缀。
        throw RpcException{-32000, e.message};
    }
    return QJsonObject{{QStringLiteral("ok"), true}};
}

QJsonValue RpcCore::rpcLogout(const QJsonObject &p)
{
    Q_UNUSED(p);
    try {
        m_app->logout();
    } catch (const RpcException &e) {
        throw RpcException{-32000, QStringLiteral("logout failed: ") + e.message};
    }
    publishEvent(QJsonObject{{QStringLiteral("event"),
                              QStringLiteral("auth.loggedOut")}});
    return QJsonObject{{QStringLiteral("ok"), true}};
}

QJsonValue RpcCore::rpcAuthStatus(const QJsonObject &)
{
    return QJsonObject{{QStringLiteral("ready"), m_app->isAuthReady()}};
}

QJsonValue RpcCore::rpcDownload(const QJsonObject &p)
{
    QStringList urls;
    const QJsonArray arr = p.value(QStringLiteral("urls")).toArray();
    for (const QJsonValue &v : arr) {
        if (v.isString())
            urls.append(v.toString());
    }
    if (urls.isEmpty())
        throw RpcException{-32602, QStringLiteral("urls is required")};

    const QPair<int, int> counts = m_app->enqueueURLs(urls);
    return QJsonObject{{QStringLiteral("enqueued"), counts.first},
                       {QStringLiteral("skipped"), counts.second}};
}

QJsonValue RpcCore::rpcSetDownloadsDirectory(const QJsonObject &p)
{
    const QString raw = strParam(p, "path");
    const QString path = raw.trimmed();
    if (path.isEmpty())
        throw RpcException{-32602, QStringLiteral("path is required")};
    try {
        m_app->setDownloadsDirectory(path);
    } catch (const RpcException &e) {
        throw RpcException{-32000,
                           QStringLiteral("set downloads directory: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("path"), raw}};
}

QJsonValue RpcCore::rpcGetDownloadsDirectory(const QJsonObject &p)
{
    QString path = strParam(p, "path");
    bool writable = true;
    QString dirError;
    if (!path.isEmpty()) {
        const Application::DirStatus d = m_app->probeDownloadsDirectory(path);
        path = d.path;
        writable = d.writable;
        dirError = d.error;
    }
    QJsonObject o;
    o["path"] = path;
    o["writable"] = writable;
    if (!writable && !dirError.isEmpty())
        o["error"] = dirError;
    return o;
}

QJsonValue RpcCore::rpcGetSettings(const QJsonObject &)
{
    const Application::SettingsView s = m_app->getSettings();
    QJsonObject o;
    o["downloads_directory"] = s.downloadsDirectory;
    o["writable"] = s.writable;
    o["quality"] = s.quality;
    o["track_exists"] = s.trackExists;
    o["track_file_template"] = s.trackFileTemplate;
    o["proxy_mode"] = s.proxyMode;
    o["proxy_url"] = s.proxyUrl;
    o["theme"] = s.theme;
    o["language"] = s.language;
    o["sort_by_genre"] = s.sortByGenre;
    o["genre_sort_mode"] = s.genreSortMode;
    if (!s.writable && !s.dirError.isEmpty())
        o["error"] = s.dirError;
    return o;
}

QJsonValue RpcCore::rpcSetQuality(const QJsonObject &p)
{
    const QString raw = strParam(p, "quality");
    try {
        m_app->setQuality(raw.trimmed());
    } catch (const RpcException &e) {
        throw RpcException{-32000, QStringLiteral("set quality: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("quality"), raw}};
}

QJsonValue RpcCore::rpcSetTrackExists(const QJsonObject &p)
{
    const QString mode = strParam(p, "mode").trimmed();
    if (mode.isEmpty())
        throw RpcException{-32602, QStringLiteral("mode is required")};
    try {
        m_app->setTrackExists(mode);
    } catch (const RpcException &e) {
        throw RpcException{-32000, QStringLiteral("set track exists: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("mode"), mode}};
}

QJsonValue RpcCore::rpcSetTrackFileTemplate(const QJsonObject &p)
{
    const QString tmpl = strParam(p, "template");
    try {
        m_app->setTrackFileTemplate(tmpl);
    } catch (const RpcException &e) {
        throw RpcException{-32000,
                           QStringLiteral("set track file template: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("template"), tmpl}};
}

QJsonValue RpcCore::rpcSetProxy(const QJsonObject &p)
{
    const QString mode = strParam(p, "mode").trimmed();
    const QString url = p.value(QStringLiteral("url")).toString();
    if (mode.isEmpty())
        throw RpcException{-32602, QStringLiteral("mode is required")};
    try {
        m_app->setProxy(mode, url);
    } catch (const RpcException &e) {
        throw RpcException{-32000, QStringLiteral("set proxy: ") + e.message};
    }
    QJsonObject o{{QStringLiteral("mode"), mode}};
    if (mode == QLatin1String("manual"))
        o["url"] = url.trimmed();
    return o;
}

QJsonValue RpcCore::rpcSetTheme(const QJsonObject &p)
{
    const QString mode = strParam(p, "mode").trimmed();
    if (mode.isEmpty())
        throw RpcException{-32602, QStringLiteral("mode is required")};
    try {
        m_app->setTheme(mode);
    } catch (const RpcException &e) {
        throw RpcException{-32000, QStringLiteral("set theme: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("mode"), mode}};
}

QJsonValue RpcCore::rpcSetLanguage(const QJsonObject &p)
{
    const QString mode = strParam(p, "mode").trimmed();
    if (mode.isEmpty())
        throw RpcException{-32602, QStringLiteral("mode is required")};
    try {
        m_app->setLanguage(mode);
    } catch (const RpcException &e) {
        throw RpcException{-32000, QStringLiteral("set language: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("mode"), mode}};
}

QJsonValue RpcCore::rpcSetGenreSorting(const QJsonObject &p)
{
    const bool enabled = p.value(QStringLiteral("enabled")).toBool(false);
    const QString mode =
        p.value(QStringLiteral("mode")).toString(QStringLiteral("genre")).trimmed();
    try {
        m_app->setGenreSorting(enabled, mode);
    } catch (const RpcException &e) {
        throw RpcException{-32000,
                           QStringLiteral("set genre sorting: ") + e.message};
    }
    return QJsonObject{{QStringLiteral("enabled"), enabled},
                       {QStringLiteral("mode"), mode}};
}

QJsonValue RpcCore::rpcStartScheduler(const QJsonObject &p)
{
    int interval = p.value(QStringLiteral("interval_seconds")).toInt(0);
    if (interval <= 0)
        interval = 3600;
    const int perPageRaw = p.value(QStringLiteral("per_page")).toInt(0);
    const QString genre = strParam(p, "genre");
    const QString dateFrom = strParam(p, "date_from");
    const QString dateTo = strParam(p, "date_to");

    SchedulerOptions opts;
    opts.intervalSeconds = interval;
    opts.genre = genre;
    opts.perPage = perPageRaw > 0 ? perPageRaw : 50;
    opts.dateFrom = dateFrom;
    opts.dateTo = dateTo;

    Scheduler::Logger logger = [this](const QString &msg) {
        publishEvent(QJsonObject{{QStringLiteral("event"), QStringLiteral("log")},
                                 {QStringLiteral("message"), msg}});
    };
    Scheduler::ResultsHandler handler = [this](const QList<Track> &tracks, QString *) {
        QStringList urls;
        urls.reserve(tracks.size());
        for (const Track &t : tracks) {
            if (!t.url.isEmpty())
                urls.append(t.url);
        }
        if (!urls.isEmpty()) {
            publishEvent(QJsonObject{{QStringLiteral("event"),
                                      QStringLiteral("scheduler.found")},
                                     {QStringLiteral("count"), urls.size()}});
            m_app->enqueueURLs(urls);
        }
    };

    {
        QMutexLocker lock(&m_schedulerMu);
        m_scheduler.reset(); // 停旧的
        m_scheduler = std::make_unique<Scheduler>(m_app->bp, opts, logger, handler);
        m_scheduler->start();
    }
    return QJsonObject{{QStringLiteral("started"), true}};
}

QJsonValue RpcCore::rpcStopScheduler(const QJsonObject &)
{
    QMutexLocker lock(&m_schedulerMu);
    m_scheduler.reset();
    return true;
}

QJsonValue RpcCore::rpcSchedulerStatus(const QJsonObject &)
{
    QMutexLocker lock(&m_schedulerMu);
    const bool running = (m_scheduler != nullptr);
    return QJsonObject{{QStringLiteral("running"), running}};
}
