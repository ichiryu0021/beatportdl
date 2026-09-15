#include "downloader.h"

#include "auth.h"
#include "cli.h"
#include "hls.h"
#include "http.h"
#include "logger.h"
#include "tags.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonValue>
#include <QMutexLocker>
#include <QSemaphore>
#include <QStringConverter>
#include <QTextStream>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <system_error>

namespace {

// 下载流水线内部错误。what() 为 UTF-8 文本（QString::toStdString），
// 用 fromWhat 还原以保证错误文本与 Go err.Error() 逐字一致。
struct PipelineError : std::runtime_error {
    explicit PipelineError(const QString &msg)
        : std::runtime_error(msg.toStdString()) {}
};

QString fromWhat(const std::exception &e)
{
    return QString::fromUtf8(e.what());
}

void throwPipe(const QString &msg)
{
    throw PipelineError(msg);
}

// RpcException 首字段是 int code（默认 -32000），不能用聚合初始化直接塞
// QString（会被拿去初始化 code），统一用本辅助抛 message。
[[noreturn]] void throwRpc(const QString &msg)
{
    RpcException ex;
    ex.message = msg;
    throw ex;
}

// filepath.Ext：最后一个路径元素中最后一个 '.' 起到结尾的部分（含点）。
QString fileExt(const QString &path)
{
    int slash = path.lastIndexOf(QLatin1Char('/'));
    const int bs = path.lastIndexOf(QLatin1Char('\\'));
    if (bs > slash)
        slash = bs;
    const int dot = path.lastIndexOf(QLatin1Char('.'));
    if (dot <= slash)
        return {};
    return path.mid(dot);
}

// 按实体模板构造 NamingPreferences；各 DirectoryName 只读取自己需要的字段。
NamingPreferences namingPrefs(const AppConfig &c, const QString &templ)
{
    NamingPreferences n;
    n.templ = templ;
    n.whitespace = c.whitespaceCharacter;
    n.artistsLimit = c.artistsLimit;
    n.artistsShortForm = c.artistsShortForm;
    n.trackNumberPadding = c.trackNumberPadding;
    n.keySystem = c.keySystem;
    return n;
}

// MkdirAll + 写探针 ".bpdl-write-test"（内容 ok）后删除。
// 对应 server_adapter.go SetDownloadsDirectory / CheckDownloadsDirectory。
// 返回空串=可写；否则为可直接抛给 RPC 层的错误文本。
QString probeWritable(const QString &rawPath)
{
    const QString path = rawPath;
    std::error_code ec;
#ifdef Q_OS_WIN
    const std::filesystem::path fsPath(reinterpret_cast<const wchar_t *>(
        QDir::toNativeSeparators(path).utf16()));
#else
    const std::filesystem::path fsPath(path.toStdString());
#endif
    std::filesystem::create_directories(fsPath, ec);
    if (ec) {
        return QStringLiteral("cannot create %1: %2")
            .arg(path, QString::fromLocal8Bit(ec.message().c_str()));
    }

    const QString probePath = QDir(path).filePath(QStringLiteral(".bpdl-write-test"));
    {
        QFile probe(probePath);
        if (!probe.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return QStringLiteral("not writable: %1").arg(probe.errorString());
        probe.write("ok");
        probe.close();
    }
    QFile::remove(probePath);
    return {};
}

// resenje.org/singleflight 的等价物：同一 release ID 的并发请求只触发一次
// GetRelease，其余 worker 共享结果/错误。
class ReleaseSingleFlight {
public:
    QSharedPointer<Release> call(qint64 id, Beatport *beat, QString *err)
    {
        std::shared_ptr<Call> call;
        bool leader = false;
        {
            QMutexLocker locker(&m_mu);
            auto it = m_pending.find(id);
            if (it == m_pending.end()) {
                call = std::make_shared<Call>();
                m_pending.emplace(id, call);
                leader = true;
            } else {
                call = it->second;
            }
        }

        if (leader) {
            call->release = beat->getRelease(id, &call->err);
            {
                std::lock_guard<std::mutex> lk(call->mu);
                call->done = true;
            }
            call->cv.notify_all();
            {
                QMutexLocker locker(&m_mu);
                m_pending.erase(id);
            }
            if (err)
                *err = call->err;
            return call->release;
        }

        {
            std::unique_lock<std::mutex> lk(call->mu);
            call->cv.wait(lk, [&call] { return call->done; });
        }
        if (err)
            *err = call->err;
        return call->release;
    }

private:
    struct Call {
        std::mutex mu;
        std::condition_variable cv;
        bool done = false;
        QString err;
        QSharedPointer<Release> release;
    };

    QMutex m_mu;
    std::map<qint64, std::shared_ptr<Call>> m_pending;
};

// ForPaginated 泛型（downloader.go）：page 从 1 起，Next 为空即停。
// processItem 与 Go 一样不返回错误（worker 内错误仅记录日志）。
template <class T, class FetchFn>
bool forPaginated(qint64 entityId, const QString &params, FetchFn fetchPage,
                  const std::function<void(const T &, int)> &processItem,
                  QString *err)
{
    int page = 1;
    while (true) {
        QSharedPointer<Paginated<T>> paginated =
            fetchPage(entityId, page, params, err);
        if (paginated.isNull()) {
            if (err)
                err->prepend(QStringLiteral("fetch page: "));
            return false;
        }

        int i = 0;
        for (const T &item : paginated->results)
            processItem(item, i++);

        if (!paginated->hasNext)
            break;
        ++page;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 构造 / Setup
// ---------------------------------------------------------------------------

Application::Application() = default;
Application::~Application() = default;

Application *Application::setup(const QString &explicitConfigPath,
                                bool serveMode, bool cliInteractive, QString *err)
{
    if (!explicitConfigPath.isEmpty())
        setConfigPathOverride(explicitConfigPath);

    bool exists = false;
    const QString configPath = findConfigFile(&exists);

    if (!exists) {
        if (cliInteractive) {
            // --serve / CLI 控制台启动（Go 行为：提示输入用户名/密码/目录/音质）。
            QString createErr;
            if (!createConfigInteractively(configPath, &createErr)) {
                if (err)
                    *err = createErr;
                return nullptr;
            }
        } else if (serveMode) {
            // 单文件 GUI 首启：WIN32 子系统没有控制台可交互（Go 的 run-gui
            // 架构里后端另开控制台窗口提问）。这里静默写一份默认配置：
            // 空凭据走延迟认证，下载目录放配置旁 downloads/，全部便携可清理。
            AppConfig boot;
            boot.applyDefaults();
            boot.downloadsDirectory =
                QFileInfo(configPath).absolutePath() + QStringLiteral("/downloads");
            QString saveErr;
            if (!boot.save(configPath, &saveErr)) {
                if (err)
                    *err = QStringLiteral("create default config: %1").arg(saveErr);
                return nullptr;
            }
            logInfo(QStringLiteral("server: created default config at %1").arg(configPath));
        }
        // 两者皆 false（非交互批处理）：交给 parseConfigFile 报文件不存在。
    }

    QString parseErr;
    QSharedPointer<AppConfig> parsed =
        parseConfigFile(configPath, /*lenient=*/serveMode, &parseErr);
    if (parsed.isNull()) {
        if (err)
            *err = QStringLiteral("load config: %1").arg(parseErr);
        return nullptr;
    }

    bool cacheExists = false;
    const QString cachePath = findCacheFile(&cacheExists);
    Q_UNUSED(cacheExists);

    std::unique_ptr<Application> app(new Application());
    app->m_configPath = configPath;
    app->m_config = std::make_shared<AppConfig>(*parsed);
    app->m_globalSem = std::make_unique<QSemaphore>(
        std::max(0, parsed->maxGlobalWorkers));
    app->m_downloadSem = std::make_unique<QSemaphore>(
        std::max(0, parsed->maxDownloadWorkers));

    if (parsed->writeErrorLog) {
        const QString logPath = findErrorLogFile(nullptr);
        auto logFile = std::make_unique<QFile>(logPath);
        if (!logFile->open(QIODevice::WriteOnly | QIODevice::Append
                           | QIODevice::Text)) {
            if (err)
                *err = QStringLiteral("cannot open error log %1: %2")
                           .arg(logPath, logFile->errorString());
            return nullptr;
        }
        app->m_errorLog = std::move(logFile);
    }

    auto a = std::make_shared<Auth>(parsed->username, parsed->password,
                                    cachePath);

    // 全局代理选路（所有出网通道共用）：在 Beatport 创建/任何请求前生效。
    ProxySetting proxy;
    if (!parseProxyMode(parsed->proxyMode, &proxy.mode))
        proxy.mode = ProxyMode::Auto;
    proxy.url = parsed->proxy.trimmed();
    setGlobalProxy(proxy);

    QString beatErr;
    QSharedPointer<Beatport> b =
        Beatport::create(a, &beatErr);
    if (b.isNull()) {
        if (err)
            *err = beatErr;
        return nullptr;
    }
    app->auth = a;
    app->bp = b;

    QString cacheErr;
    if (a->loadCache(&cacheErr)) {
        QMutexLocker locker(&app->m_authMu);
        app->m_authReady = true;
    } else {
        // 缓存失效（含 ErrLoginIDMismatch）：重新 Init。
        if (serveMode) {
            // 延迟认证：Init 失败不致命，继续起服务等待前端 login RPC。
            if (!parsed->username.isEmpty() && !parsed->password.isEmpty()) {
                QString initErr;
                if (a->init(b.get(), &initErr)) {
                    QMutexLocker locker(&app->m_authMu);
                    app->m_authReady = true;
                } else {
                    logInfo(QStringLiteral("server: auth not ready: %1")
                                .arg(initErr));
                }
            } else {
                logInfo(QStringLiteral(
                    "server: no credentials in config; waiting for GUI login"));
            }
        } else {
            QString initErr;
            if (!a->init(b.get(), &initErr)) {
                app->appendErrorLogLine(
                    QStringLiteral("beatport: %1").arg(initErr));
                if (err)
                    *err = initErr;
                return nullptr;
            }
        }
    }

    // checker 总是创建；verify_integrity 门控在 saveTrack 调用处。
    app->m_checker = std::make_unique<Checker>();
    app->rebuildArchiver();

    return app.release();
}

// ---------------------------------------------------------------------------
// 配置快照 / 持久化 / archiver
// ---------------------------------------------------------------------------

std::shared_ptr<AppConfig> Application::snapshotConfig() const
{
    QMutexLocker locker(&m_settingsMu);
    return m_config;
}

std::shared_ptr<Archiver> Application::snapshotArchiver()
{
    QMutexLocker locker(&m_archiverMu);
    return m_archiver;
}

QString Application::saveConfigLocked()
{
    QMutexLocker locker(&m_configSaveMu);
    if (m_configPath.isEmpty())
        return {};
    QString errText;
    if (!m_config->save(m_configPath, &errText))
        return errText;
    return {};
}

void Application::rebuildArchiver()
{
    const auto cfg = snapshotConfig();
    QMutexLocker locker(&m_archiverMu);
    if (!cfg->sortByGenre) {
        m_archiver.reset();
        return;
    }
    const GenreMode mode =
        cfg->genreSortMode == QStringLiteral("subgenre_or_genre")
            ? GenreMode::SubgenreOrGenre
            : GenreMode::Genre;
    const QChar whitespace = cfg->whitespaceCharacter.isEmpty()
                                 ? QChar()
                                 : cfg->whitespaceCharacter.at(0);
    m_archiver = std::make_shared<Archiver>(cfg->downloadsDirectory, mode,
                                            whitespace);
}

// ---------------------------------------------------------------------------
// server 适配层（server_adapter.go）
// ---------------------------------------------------------------------------

bool Application::isAuthReady()
{
    QMutexLocker locker(&m_authMu);
    return m_authReady;
}

void Application::login(const QString &username, const QString &password)
{
    if (username.isEmpty() || password.isEmpty())
        throwRpc(QStringLiteral("username and password are required"));

    QString cachePath;
    {
        QMutexLocker locker(&m_authMu);
        if (auth)
            cachePath = auth->cacheFile();
    }

    auto newAuth = std::make_shared<Auth>(username, password, cachePath);
    QString initErr;
    if (!newAuth->init(bp.get(), &initErr))
        throwRpc(initErr);

    {
        QMutexLocker locker(&m_authMu);
        auth = newAuth;
        bp->setAuth(newAuth);
        {
            QMutexLocker settingsLocker(&m_settingsMu);
            auto next = std::make_shared<AppConfig>(*m_config);
            next->username = username;
            next->password = password;
            m_config = next;
        }
        m_authReady = true;
    }

    // 写不回去不影响当前会话，只记录。
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        logInfo(QStringLiteral("server: save config after login: %1")
                    .arg(saveErr));
}

void Application::logout()
{
    QMutexLocker authLocker(&m_authMu);

    {
        QMutexLocker settingsLocker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->username.clear();
        next->password.clear();
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        logInfo(QStringLiteral("server: save config after logout: %1")
                    .arg(saveErr));

    if (auth) {
        const QString cachePath = auth->cacheFile();
        if (!cachePath.isEmpty()) {
            QFile cacheFile(cachePath);
            // Go 忽略文件不存在（os.IsNotExist），其余错误记录。
            if (cacheFile.exists() && !cacheFile.remove()
                && cacheFile.exists()) {
                logInfo(QStringLiteral("server: remove auth cache: %1")
                            .arg(cacheFile.errorString()));
            }
        }
    }

    m_authReady = false;
}

Application::DirStatus
Application::probeDownloadsDirectory(const QString &path) const
{
    // 与 Go CheckDownloadsDirectory 一致：空路径时 MkdirAll("") 成功、
    // 探针 ".bpdl-write-test" 落在当前工作目录，因此同样直接探测。
    DirStatus status;
    status.path = path;
    const QString errText = probeWritable(path);
    if (errText.isEmpty()) {
        status.writable = true;
    } else {
        status.writable = false;
        status.error = errText;
    }
    return status;
}

void Application::setDownloadsDirectory(const QString &rawPath)
{
    const QString path = rawPath.trimmed();
    if (path.isEmpty())
        throwRpc(QStringLiteral("path is required"));

    const QString errText = probeWritable(path);
    if (!errText.isEmpty())
        throwRpc(errText);

    bool rebuild = false;
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->downloadsDirectory = path;
        rebuild = next->sortByGenre;
        m_config = next;
    }

    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        logInfo(QStringLiteral(
                    "server: save config after set downloads dir: %1")
                    .arg(saveErr));

    if (rebuild)
        rebuildArchiver();
}

Application::SettingsView Application::getSettings()
{
    const auto cfg = snapshotConfig();
    const DirStatus dir = probeDownloadsDirectory(cfg->downloadsDirectory);
    return {cfg->downloadsDirectory, dir.writable, dir.error,
            cfg->quality, cfg->trackExists, cfg->trackFileTemplate,
            cfg->proxyMode, cfg->proxy, cfg->theme, cfg->language,
            cfg->sortByGenre, cfg->genreSortMode};
}

void Application::setQuality(const QString &quality)
{
    static const QStringList kAllowed{
        QStringLiteral("lossless"), QStringLiteral("high"),
        QStringLiteral("medium"), QStringLiteral("medium-hls")};
    if (!kAllowed.contains(quality)) {
        throwRpc(QStringLiteral("unsupported quality \"%1\" (want "
                                "lossless/high/medium/medium-hls)")
                     .arg(quality));
    }
    if (quality == QLatin1String("medium-hls") && !ffmpegInstalled()) {
        throwRpc(QStringLiteral(
            "quality medium-hls requires ffmpeg, which was not found in PATH"));
    }
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->quality = quality;
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(QStringLiteral("save config after set quality: %1")
                     .arg(saveErr));
}

void Application::setTrackExists(const QString &mode)
{
    if (mode.isEmpty())
        throwRpc(QStringLiteral("mode is required"));
    if (!kSupportedTrackExistsOptions.contains(mode)) {
        throwRpc(QStringLiteral("unsupported track_exists mode \"%1\" "
                                "(want one of %2)")
                     .arg(mode,
                          kSupportedTrackExistsOptions.join(
                              QStringLiteral(", "))));
    }
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->trackExists = mode;
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(QStringLiteral("save config after set track exists: %1")
                     .arg(saveErr));
}

void Application::setTrackFileTemplate(const QString &rawTemplate)
{
    const QString tmpl = rawTemplate.trimmed();
    if (tmpl.isEmpty())
        throwRpc(QStringLiteral("track file template is empty"));
    if (!tmpl.contains(QStringLiteral("{name}"))) {
        throwRpc(QStringLiteral(
                     "track file template \"%1\" must contain {name}")
                     .arg(tmpl));
    }
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->trackFileTemplate = tmpl;
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(QStringLiteral(
                     "save config after set track file template: %1")
                     .arg(saveErr));
}

void Application::setProxy(const QString &modeRaw, const QString &urlRaw)
{
    const QString mode = modeRaw.trimmed();
    ProxyMode pm;
    if (!parseProxyMode(mode, &pm))
        throwRpc(QStringLiteral("unsupported proxy mode \"%1\" (want off/auto/manual)")
                     .arg(modeRaw));

    const QString url = urlRaw.trimmed();
    if (pm == ProxyMode::Manual) {
        if (url.isEmpty())
            throwRpc(QStringLiteral("manual proxy url is required"));
        const QUrl pu(url);
        const QString scheme = pu.scheme().toLower();
        if (!pu.isValid() || pu.host().isEmpty() || pu.port() <= 0
            || (scheme != QLatin1String("http")
                && scheme != QLatin1String("https")
                && !scheme.startsWith(QLatin1String("socks")))) {
            throwRpc(QStringLiteral(
                         "invalid proxy url \"%1\" (want http://host:port "
                         "or socks5://host:port)")
                         .arg(urlRaw));
        }
    }

    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->proxyMode = mode;
        next->proxy = pm == ProxyMode::Manual ? url : QString();
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(QStringLiteral("save config after set proxy: %1").arg(saveErr));

    // 即时生效：更新全局选路，各工作线程的 thread_local QNAM 下一次请求重配。
    ProxySetting setting;
    setting.mode = pm;
    setting.url = pm == ProxyMode::Manual ? url : QString();
    setGlobalProxy(setting);
}

void Application::setTheme(const QString &modeRaw)
{
    const QString mode = modeRaw.trimmed();
    if (mode != QLatin1String("light") && mode != QLatin1String("dark"))
        throwRpc(QStringLiteral("unsupported theme \"%1\" (want light/dark)")
                     .arg(modeRaw));
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->theme = mode;
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(QStringLiteral("save config after set theme: %1").arg(saveErr));
}

void Application::setLanguage(const QString &modeRaw)
{
    const QString mode = modeRaw.trimmed();
    if (mode != QLatin1String("system") && mode != QLatin1String("en") &&
        mode != QLatin1String("zh"))
        throwRpc(QStringLiteral("unsupported language \"%1\" (want system/en/zh)")
                     .arg(modeRaw));
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->language = mode;
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(QStringLiteral("save config after set language: %1").arg(saveErr));
}

void Application::setGenreSorting(bool enabled, const QString &modeRaw)
{
    const QString mode = modeRaw.trimmed();
    if (enabled &&
        mode != QLatin1String("genre") &&
        mode != QLatin1String("subgenre_or_genre"))
        throwRpc(QStringLiteral(
                     "unsupported genre_sort_mode \"%1\" "
                     "(want genre/subgenre_or_genre)")
                     .arg(modeRaw));
    {
        QMutexLocker locker(&m_settingsMu);
        auto next = std::make_shared<AppConfig>(*m_config);
        next->sortByGenre = enabled;
        if (enabled)
            next->genreSortMode = mode;
        m_config = next;
    }
    const QString saveErr = saveConfigLocked();
    if (!saveErr.isEmpty())
        throwRpc(
            QStringLiteral("save config after set genre sorting: %1").arg(saveErr));
    rebuildArchiver();
}

QPair<int, int> Application::enqueueURLs(const QStringList &urls)
{
    if (urls.isEmpty())
        return {0, 0};

    QStringList skipped;
    int enqueued = 0;

    // pending + 当前活动批次 + 本次调用三方去重，全部在同一把锁内判定。
    {
        QMutexLocker locker(&m_urlsMu);
        QSet<QString> seen;
        seen.reserve(m_pendingURLs.size() + m_activeURLs.size() + urls.size());
        for (const QString &u : m_pendingURLs)
            seen.insert(u);
        for (const QString &u : m_activeURLs)
            seen.insert(u);
        for (const QString &u : urls) {
            if (seen.contains(u)) {
                skipped.append(u);
                continue;
            }
            seen.insert(u);
            m_pendingURLs.append(u);
            ++enqueued;
        }
    }

    // 锁外广播。
    for (const QString &u : skipped) {
        QJsonObject extra;
        extra[QStringLiteral("reason")] = QStringLiteral("duplicate");
        publishDownloadEvent(QStringLiteral("download.skipped"), u,
                             QString(), extra);
    }
    if (enqueued > 0) {
        logInfo(QStringLiteral("server: enqueued %1 urls (%2 duplicates "
                               "skipped)")
                    .arg(enqueued)
                    .arg(skipped.size()));
    }
    return {enqueued, skipped.size()};
}

QStringList Application::drainPending()
{
    QMutexLocker locker(&m_urlsMu);
    m_activeURLs += m_pendingURLs;
    m_pendingURLs.clear();
    return m_activeURLs;
}

void Application::resetActiveURLs()
{
    QMutexLocker locker(&m_urlsMu);
    m_activeURLs.clear();
}

// ---------------------------------------------------------------------------
// 主循环
// ---------------------------------------------------------------------------

void Application::serveLoop()
{
    while (!stopFlag.load()) {
        const QStringList urls = drainPending();
        if (urls.isEmpty()) {
            QThread::msleep(250);
            continue;
        }

        {
            QMutexLocker locker(&m_activeFilesMu);
            m_activeFiles.clear();
        }

        WaitGroup wg;
        for (const QString &u : urls) {
            globalWorker(&wg, [this, u] { handleUrl(u); });
        }
        wg.wait();

        // 批次完成后释放 URL，使其可以被再次入队。
        resetActiveURLs();
    }
}

void Application::runOnce()
{
    const QStringList urls = drainPending();
    if (urls.isEmpty())
        return;

    {
        QMutexLocker locker(&m_activeFilesMu);
        m_activeFiles.clear();
    }

    WaitGroup wg;
    for (const QString &u : urls) {
        globalWorker(&wg, [this, u] { handleUrl(u); });
    }
    wg.wait();

    resetActiveURLs();
}

// ---------------------------------------------------------------------------
// worker 池（concurrency.go）
// ---------------------------------------------------------------------------

void Application::globalWorker(WaitGroup *wg, const std::function<void()> &fn)
{
    wg->add(1);

    std::thread([this, wg, fn]() {
        m_globalSem->acquire();
        struct Guards {
            QSemaphore *sem;
            WaitGroup *wg;
            ~Guards()
            {
                sem->release(); // 与 Go defer 顺序一致：先 release 再 Done
                wg->done();
            }
        } guards{m_globalSem.get(), wg};

        try {
            fn();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "panic recovered: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "panic recovered\n");
        }
    }).detach();
}

void Application::downloadWorker(WaitGroup *wg, const std::function<void()> &fn)
{
    wg->add(1);

    std::thread([this, wg, fn]() {
        struct DoneGuard {
            WaitGroup *wg;
            ~DoneGuard() { wg->done(); }
        } doneGuard{wg};

        // 获取 downloadSem 之前先查停止信号（Go select ctx.Done()）。
        if (stopFlag.load())
            return;

        m_downloadSem->acquire();
        struct SemGuard {
            QSemaphore *sem;
            ~SemGuard() { sem->release(); }
        } semGuard{m_downloadSem.get()};

        try {
            fn();
        } catch (const std::exception &e) {
            std::fprintf(stderr, "panic recovered: %s\n", e.what());
        } catch (...) {
            std::fprintf(stderr, "panic recovered\n");
        }
    }).detach();
}

// ---------------------------------------------------------------------------
// 事件 / 日志
// ---------------------------------------------------------------------------

void Application::publishEvent(const QJsonObject &ev)
{
    if (m_sink)
        m_sink->publishEvent(ev);
}

void Application::publishDownloadEvent(const QString &event, const QString &url,
                                       const QString &title,
                                       const QJsonObject &extra)
{
    QJsonObject payload;
    payload[QStringLiteral("event")] = event;
    payload[QStringLiteral("url")] = url;
    payload[QStringLiteral("title")] = title;
    for (auto it = extra.constBegin(); it != extra.constEnd(); ++it)
        payload.insert(it.key(), it.value());
    publishEvent(payload);
}

void Application::appendErrorLogLine(const QString &line)
{
    if (!m_errorLog)
        return;
    QMutexLocker locker(&m_errorLogMu);
    if (m_errorLog->isOpen()) {
        QTextStream out(m_errorLog.get());
        out.setEncoding(QStringConverter::Utf8);
        out << line << QLatin1Char('\n');
        m_errorLog->flush();
    }
}

void Application::logErrorEx(const QString &caller, const QString &errText)
{
    logErrorLine(caller, errText);          // logs/backend.log（+控制台镜像）
    appendErrorLogLine(QStringLiteral("%1: %2").arg(caller, errText));
}

void Application::errorLogWrapper(const QString &url, const QString &step,
                                 const QString &errText)
{
    logErrorEx(QStringLiteral("[%1] %2").arg(url, step), errText);
}

void Application::infoLogWrapper(const QString &url, const QString &message)
{
    logInfo(QStringLiteral("[%1] %2").arg(url, message));
}

void Application::publishUrlError(const QString &url, const QString &step,
                                  const QString &errText)
{
    errorLogWrapper(url, step, errText);
    QJsonObject extra;
    extra[QStringLiteral("error")] =
        QStringLiteral("%1: %2").arg(step, errText);
    publishDownloadEvent(QStringLiteral("download.error"), url, QString(),
                         extra);
}

// ---------------------------------------------------------------------------
// 活动文件名冲突表
// ---------------------------------------------------------------------------

bool Application::registerActiveFile(const QString &path)
{
    QMutexLocker locker(&m_activeFilesMu);
    if (m_activeFiles.contains(path))
        return false;
    m_activeFiles.insert(path);
    return true;
}

void Application::unregisterActiveFile(const QString &path)
{
    QMutexLocker locker(&m_activeFilesMu);
    m_activeFiles.remove(path);
}

QString Application::uniqueActivePath(const QString &path) const
{
    const QFileInfo fi(path);
    const QString dir = fi.path();
    const QString fullBase = fi.fileName();
    QString ext;
    if (!fi.suffix().isEmpty())
        ext = QLatin1Char('.') + fi.suffix();
    QString stem = fullBase;
    if (!ext.isEmpty())
        stem.chop(ext.size());

    for (int i = 1;; ++i) {
        const QString candidate = QDir(dir).filePath(
            QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(ext));
        if (!QFileInfo::exists(candidate))
            return candidate;
    }
}

// ---------------------------------------------------------------------------
// 目录 / 封面
// ---------------------------------------------------------------------------

QString Application::setupDownloadsDirectory(const ConfigPtr &cfg,
                                             const QString &baseDirIn,
                                             const QString &subDir,
                                             bool releaseEntity,
                                             const QString &labelName)
{
    QString baseDir = baseDirIn;
    if (cfg->sortByContext) {
        if (releaseEntity && cfg->sortByLabel)
            baseDir = QDir(baseDir).filePath(labelName);
        baseDir = QDir(baseDir).filePath(subDir);
    }
    QString errText;
    if (!createDirectory(baseDir, &errText))
        throwPipe(errText);
    return baseDir;
}

void Application::cleanupDirectory(const QString &dir, const ConfigPtr &cfg)
{
    // Go 用 os.Remove：仅当目录为空时删除，非空返回错误并被忽略。
    if (dir != cfg->downloadsDirectory)
        QDir().rmdir(dir);
}

bool Application::requireCover(bool respectFixTags, bool respectKeepCover,
                               const ConfigPtr &cfg)
{
    const bool fixTags =
        respectFixTags && cfg->fixTags
        && (cfg->coverSize != kDefaultCoverSize
            || cfg->quality != QLatin1String("lossless"));
    const bool keepCover =
        respectKeepCover && cfg->sortByContext && cfg->keepCover;
    return fixTags || keepCover;
}

QString Application::downloadCover(const QString &dir, const Image &image,
                                   const ConfigPtr &cfg, QString *err)
{
    const QString coverUrl = image.formattedUrl(cfg->coverSize);
    const QString coverPath = QDir(dir).filePath(newUuidString());

    // Go downloadCover 调 app.downloadFile（无进度回调）；这里直接调
    // downloadToFile 以保留底层错误原文（只记日志，不抛异常）。
    QString errText;
    if (!downloadToFile(coverUrl, coverPath, nullptr, &errText)) {
        QFile::remove(coverPath);
        if (err)
            *err = errText;
        return {};
    }
    return coverPath;
}

void Application::handleCoverFile(const QString &path, const ConfigPtr &cfg)
{
    if (path.isEmpty())
        return;
    if (cfg->keepCover && cfg->sortByContext) {
        const QString newPath =
            QFileInfo(path).path() + QStringLiteral("/cover.jpg");
        QFile f(path);
        if (!f.rename(newPath))
            throwPipe(f.errorString());
    } else {
        QFile::remove(path);
    }
}

bool Application::downloadFile(const QString &url, const QString &destination,
                               const QString &progressPrefix)
{
    QString errText;
    bool ok;
    if (progressPrefix.isEmpty()) {
        ok = downloadToFile(url, destination, nullptr, &errText);
    } else {
        ok = downloadToFile(
            url, destination,
            [this, progressPrefix](qint64 got, qint64 total) {
                if (useConsoleProgress) {
                    const int pct = total > 0 ? int(got * 100 / total) : 0;
                    consoleProgress(progressPrefix, pct);
                }
            },
            &errText);
    }
    return ok;
}

// ---------------------------------------------------------------------------
// saveTrack
// ---------------------------------------------------------------------------

QString Application::saveTrack(const Track &track, const QString &directory,
                               const QString &eventURLIn, const ConfigPtr &cfg)
{
    QString eventURL = eventURLIn;
    if (eventURL.isEmpty())
        eventURL = track.storeUrl();

    QString fileExtension;
    QString displayQuality;

    QSharedPointer<TrackDownload> download;
    QSharedPointer<TrackStream> stream;

    if (cfg->quality == QLatin1String("medium-hls")) {
        QString errText;
        stream = bp->streamTrack(track.id, &errText);
        if (stream.isNull())
            throwPipe(errText);
        fileExtension = QStringLiteral(".m4a");
        displayQuality = QStringLiteral("AAC 128kbps - HLS");
    } else {
        QString errText;
        download = bp->downloadTrack(track.id, cfg->quality, &errText);
        if (download.isNull())
            throwPipe(errText);
        const QString sq = download->streamQuality;
        if (sq == QLatin1String(".128k.aac.mp4")) {
            fileExtension = QStringLiteral(".m4a");
            displayQuality = QStringLiteral("AAC 128kbps");
        } else if (sq == QLatin1String(".256k.aac.mp4")) {
            fileExtension = QStringLiteral(".m4a");
            displayQuality = QStringLiteral("AAC 256kbps");
        } else if (sq == QLatin1String(".flac")) {
            fileExtension = QStringLiteral(".flac");
            displayQuality = QStringLiteral("FLAC");
        } else {
            throwPipe(QStringLiteral("invalid stream quality: %1").arg(sq));
        }
    }

    const NamingPreferences prefs =
        namingPrefs(*cfg, cfg->trackFileTemplate);
    const QString fileName = track.filename(prefs);
    QString filePath =
        QDir(directory).filePath(fileName + fileExtension);

    if (QFileInfo::exists(filePath)) {
        bool active = false;
        {
            QMutexLocker locker(&m_activeFilesMu);
            active = m_activeFiles.contains(filePath);
        }

        if (active) {
            filePath = uniqueActivePath(filePath);
        } else {
            if (cfg->trackExists == QLatin1String("skip"))
                return {};
            if (cfg->trackExists == QLatin1String("update")) {
                infoLogWrapper(track.storeUrl(),
                               QStringLiteral("updating tags"));
                return filePath;
            }
            if (cfg->trackExists == QLatin1String("error"))
                throwPipe(QStringLiteral("file already exists"));
            // overwrite：无特殊处理，继续覆盖。
        }
    }

    {
        QMutexLocker locker(&m_activeFilesMu);
        m_activeFiles.insert(filePath);
    }

    const QString infoDisplay =
        QStringLiteral("%1 (%2) [%3]")
            .arg(track.name, track.mixName, displayQuality);
    QString prefix;
    if (cfg->showProgress) {
        prefix = infoDisplay;
    } else {
        logInfo(QStringLiteral("Downloading %1").arg(infoDisplay));
    }

    if (!download.isNull()) {
        QString errText;
        const QString title = infoDisplay;
        const bool ok = downloadToFile(
            download->location, filePath,
            [this, &eventURL, &title, &prefix](qint64 received, qint64 total) {
                int pct = 0;
                if (total > 0)
                    pct = int(received * 100 / total);
                QJsonObject extra;
                extra[QStringLiteral("percent")] = pct;
                extra[QStringLiteral("received")] = qint64(received);
                extra[QStringLiteral("total")] = qint64(total);
                publishDownloadEvent(QStringLiteral("download.progress"),
                                     eventURL, title, extra);
                if (useConsoleProgress && !prefix.isEmpty())
                    consoleProgress(prefix, pct);
            },
            &errText);
        if (!ok) {
            QFile::remove(filePath);
            throwPipe(errText);
        }
        if (useConsoleProgress && !prefix.isEmpty())
            consoleProgressDone(prefix);
    } else if (!stream.isNull()) {
        QStringList segmentUrls;
        HlsKey key;
        QString errText;
        if (!getStreamSegments(stream->url, &segmentUrls, &key, &errText))
            throwPipe(QStringLiteral("get stream segments: %1").arg(errText));

        const QString segmentsFile =
            QDir(directory).filePath(newUuidString());

        QString segErr;
        const bool ok = downloadSegments(
            segmentUrls, key, segmentsFile,
            [this, &prefix](int done, int total, qint64) {
                if (useConsoleProgress && !prefix.isEmpty() && total > 0)
                    consoleProgress(prefix, done * 100 / total);
            },
            &segErr);
        if (!ok) {
            QFile::remove(segmentsFile);
            throwPipe(
                QStringLiteral("download segments: %1").arg(segErr));
        }

        QString remuxErr;
        if (!remuxToM4A(segmentsFile, filePath, &remuxErr)) {
            QFile::remove(filePath);
            QFile::remove(segmentsFile);
            throwPipe(QStringLiteral("remux to m4a: %1").arg(remuxErr));
        }
        QFile::remove(segmentsFile);

        if (useConsoleProgress && !prefix.isEmpty())
            consoleProgressDone(prefix);
    }

    if (!cfg->showProgress)
        logInfo(QStringLiteral("Finished downloading %1").arg(infoDisplay));

    if (m_checker && cfg->verifyIntegrity) {
        const IntegrityResult result = m_checker->verifyAudio(filePath);
        if (!result.valid) {
            errorLogWrapper(track.storeUrl(),
                            QStringLiteral("integrity check"), result.error);
            QFile::remove(filePath);
            throwPipe(QStringLiteral("integrity check failed: %1")
                          .arg(result.error));
        }
        infoLogWrapper(
            track.storeUrl(),
            QStringLiteral("verified: %1 bytes, %2 Hz")
                .arg(result.actualBytes)
                .arg(result.sampleRate));
    }

    return filePath;
}

// ---------------------------------------------------------------------------
// tagTrack（tag_mappings.go 30 个 mappingValues 严格对齐）
// ---------------------------------------------------------------------------

void Application::tagTrack(const QString &location, const Track &track,
                           const QString &coverPath, const ConfigPtr &cfg)
{
    const QString ext = fileExt(location);
    if (!cfg->fixTags)
        return;

    QString errText;
    std::unique_ptr<TaggedFile> file = TaggedFile::read(location, &errText);
    if (!file)
        throwPipe(errText);

    const QString subgenre =
        track.subgenre.has_value() ? track.subgenre->name : QString();

    QMap<QString, QString> v;
    v[QStringLiteral("track_id")] = QString::number(track.id);
    v[QStringLiteral("track_url")] = track.storeUrl();
    v[QStringLiteral("track_name")] =
        QStringLiteral("%1 (%2)").arg(track.name, track.mixName);
    v[QStringLiteral("track_artists")] = track.artists.display(0, QString());
    v[QStringLiteral("track_remixers")] =
        track.remixers.display(0, QString());
    v[QStringLiteral("track_artists_limited")] =
        track.artists.display(cfg->artistsLimit, cfg->artistsShortForm);
    v[QStringLiteral("track_remixers_limited")] =
        track.remixers.display(cfg->artistsLimit, cfg->artistsShortForm);
    v[QStringLiteral("track_number")] = QString::number(track.number);
    v[QStringLiteral("track_number_with_padding")] =
        numberWithPadding(track.number, track.release.trackCount,
                          cfg->trackNumberPadding);
    v[QStringLiteral("track_number_with_total")] =
        QStringLiteral("%1/%2").arg(track.number).arg(track.release.trackCount);
    v[QStringLiteral("track_genre")] = track.genre.name;
    v[QStringLiteral("track_subgenre")] = subgenre;
    v[QStringLiteral("track_genre_with_subgenre")] =
        track.genreWithSubgenre(QStringLiteral("|"));
    v[QStringLiteral("track_subgenre_or_genre")] = track.subgenreOrGenre();
    v[QStringLiteral("track_key")] = track.key.display(cfg->keySystem);
    v[QStringLiteral("track_bpm")] = QString::number(track.bpm);
    v[QStringLiteral("track_isrc")] = track.isrc;

    v[QStringLiteral("release_id")] = QString::number(track.release.id);
    v[QStringLiteral("release_url")] = track.release.storeUrl();
    v[QStringLiteral("release_name")] = track.release.name;
    v[QStringLiteral("release_artists")] =
        track.release.artists.display(0, QString());
    v[QStringLiteral("release_remixers")] =
        track.release.remixers.display(0, QString());
    v[QStringLiteral("release_artists_limited")] =
        track.release.artists.display(cfg->artistsLimit,
                                     cfg->artistsShortForm);
    v[QStringLiteral("release_remixers_limited")] =
        track.release.remixers.display(cfg->artistsLimit,
                                      cfg->artistsShortForm);
    v[QStringLiteral("release_date")] = track.release.date;
    v[QStringLiteral("release_year")] = track.release.year();
    v[QStringLiteral("release_track_count")] =
        QString::number(track.release.trackCount);
    v[QStringLiteral("release_track_count_with_padding")] =
        numberWithPadding(track.release.trackCount,
                          track.release.trackCount,
                          cfg->trackNumberPadding);
    v[QStringLiteral("release_catalog_number")] = track.release.catalogNumber;
    v[QStringLiteral("release_upc")] = track.release.upc;
    v[QStringLiteral("release_label")] = track.release.label.name;
    v[QStringLiteral("release_label_url")] = track.release.label.storeUrl();

    if (ext == QLatin1String(".m4a")) {
        if (!file->stripMp4(&errText))
            throwPipe(errText);
    } else {
        const QStringList existingTags = file->propertyKeys();
        for (const QString &tag : existingTags)
            file->setProperty(tag, QString(), nullptr);
    }

    if (ext == QLatin1String(".flac")) {
        const auto fmtIt = cfg->tagMappings.constFind(QStringLiteral("flac"));
        if (fmtIt != cfg->tagMappings.constEnd()) {
            for (auto it = fmtIt->constBegin(); it != fmtIt->constEnd(); ++it) {
                const QString value = v.value(it.key());
                if (!value.isEmpty())
                    file->setProperty(it.value(), value, nullptr);
            }
        }
    } else if (ext == QLatin1String(".m4a")) {
        QMap<QString, QString> rawTags;

        const auto fmtIt = cfg->tagMappings.constFind(QStringLiteral("m4a"));
        if (fmtIt != cfg->tagMappings.constEnd()) {
            for (auto it = fmtIt->constBegin(); it != fmtIt->constEnd(); ++it) {
                QString property = it.value();
                const QString value = v.value(it.key());
                if (property.endsWith(QLatin1String("_raw"))) {
                    if (!value.isEmpty()) {
                        property.chop(4);
                        rawTags.insert(property, value);
                    }
                } else if (!value.isEmpty()) {
                    file->setProperty(it.value(), value, nullptr);
                }
            }
        }

        for (auto it = rawTags.constBegin(); it != rawTags.constEnd(); ++it)
            file->setItemMp4(it.key(), it.value(), nullptr);
    }

    if (!coverPath.isEmpty()
        && (cfg->coverSize != kDefaultCoverSize
            || ext == QLatin1String(".m4a"))) {
        QFile coverFile(coverPath);
        if (!coverFile.open(QIODevice::ReadOnly))
            throwPipe(coverFile.errorString());
        const QByteArray data = coverFile.readAll();
        coverFile.close();
        if (!file->setCoverJpeg(data, &errText))
            throwPipe(errText);
    }

    if (!file->save(&errText))
        throwPipe(errText);
}

// ---------------------------------------------------------------------------
// handleTrack
// ---------------------------------------------------------------------------

void Application::handleTrack(const Track &track, const QString &sourceUrl,
                              const QString &downloadsDir,
                              const QString &coverPath, const ConfigPtr &cfg)
{
    QString eventURL = sourceUrl;
    if (eventURL.isEmpty())
        eventURL = track.storeUrl();

    const QString title =
        QStringLiteral("%1 (%2)").arg(track.name, track.mixName);
    publishDownloadEvent(QStringLiteral("download.started"), eventURL, title);

    QString location;
    try {
        location = saveTrack(track, downloadsDir, eventURL, cfg);
    } catch (const std::exception &e) {
        QJsonObject extra;
        extra[QStringLiteral("error")] = fromWhat(e);
        publishDownloadEvent(QStringLiteral("download.error"), eventURL, title,
                             extra);
        throw PipelineError(
            QStringLiteral("save track: %1").arg(fromWhat(e)));
    }

    // saveTrack 返回空串仅发生在 track_exists=skip 且文件已存在时。
    if (location.isEmpty()) {
        QJsonObject extra;
        extra[QStringLiteral("reason")] = QStringLiteral("exists");
        publishDownloadEvent(QStringLiteral("download.skipped"), eventURL,
                             title, extra);
        return;
    }

    try {
        tagTrack(location, track, coverPath, cfg);
    } catch (const std::exception &e) {
        QJsonObject extra;
        extra[QStringLiteral("error")] = fromWhat(e);
        publishDownloadEvent(QStringLiteral("download.error"), eventURL, title,
                             extra);
        throw PipelineError(
            QStringLiteral("tag track: %1").arg(fromWhat(e)));
    }

    // sort_by_genre：把成品移入 <downloads>/<genre>/。失败仅日志。
    const auto archiver = snapshotArchiver();
    if (archiver) {
        QString fileErr;
        const QString moved = archiver->file(track, location, &fileErr);
        if (!fileErr.isEmpty()) {
            errorLogWrapper(eventURL, QStringLiteral("genre filing"), fileErr);
        } else if (moved != location) {
            const QString genreDir =
                QFileInfo(QFileInfo(moved).path()).fileName();
            infoLogWrapper(
                eventURL,
                QStringLiteral("filed into genre: %1").arg(genreDir));
        }
    }

    // 注意：事件 path 用归档前的 location（与 Go 一致）。
    QJsonObject extra;
    extra[QStringLiteral("path")] = location;
    publishDownloadEvent(QStringLiteral("download.done"), eventURL, title,
                         extra);
}

// ---------------------------------------------------------------------------
// handleUrl / 六种 link handler
// ---------------------------------------------------------------------------

void Application::handleUrl(const QString &url)
{
    Link link;
    QString errText;
    if (!bp->parseUrl(url, &link, &errText)) {
        errorLogWrapper(url, QStringLiteral("parse url"), errText);
        return;
    }

    switch (link.type) {
    case LinkType::Track:
        handleTrackLink(link, link.original);
        break;
    case LinkType::Release:
        handleReleaseLink(link);
        break;
    case LinkType::Playlist:
        handlePlaylistLink(link);
        break;
    case LinkType::Chart:
        handleChartLink(link);
        break;
    case LinkType::Label:
        handleLabelLink(link);
        break;
    case LinkType::Artist:
        handleArtistLink(link);
        break;
    default:
        logErrorEx(QStringLiteral("handle URL"),
                   QStringLiteral("unsupported link type"));
    }
}

void Application::handleTrackLink(const Link &link, const QString &sourceUrl)
{
    const auto cfg = snapshotConfig();
    const QSharedPointer<Beatport> beat = bp;

    QString errText;
    QSharedPointer<Track> track = beat->getTrack(link.id, &errText);
    if (track.isNull()) {
        errorLogWrapper(sourceUrl, QStringLiteral("fetch track"), errText);
        return;
    }

    QSharedPointer<Release> release =
        beat->getRelease(track->release.id, &errText);
    if (release.isNull()) {
        errorLogWrapper(sourceUrl, QStringLiteral("fetch track release"),
                        errText);
        return;
    }
    track->release = *release;

    QString downloadsDir;
    try {
        downloadsDir = setupDownloadsDirectory(
            cfg, cfg->downloadsDirectory,
            release->directoryName(
                namingPrefs(*cfg, cfg->releaseDirectoryTemplate)),
            /*releaseEntity=*/true, release->label.name);
    } catch (const std::exception &e) {
        publishUrlError(sourceUrl,
                        QStringLiteral("setup downloads directory"),
                        fromWhat(e));
        return;
    }

    const auto wg = std::make_shared<WaitGroup>();
    downloadWorker(wg.get(),
                   [this, cfg, beat, track, downloadsDir, sourceUrl]() {
                       QString cover;
                       if (requireCover(true, true, cfg)) {
                           QString coverErr;
                           cover = downloadCover(
                               downloadsDir, track->release.image, cfg,
                               &coverErr);
                           if (!coverErr.isEmpty())
                               errorLogWrapper(
                                   sourceUrl,
                                   QStringLiteral(
                                       "download track release cover"),
                                   coverErr);
                       }

                       try {
                           handleTrack(*track, sourceUrl, downloadsDir, cover,
                                       cfg);
                       } catch (const std::exception &e) {
                           errorLogWrapper(sourceUrl,
                                           QStringLiteral("handle track"),
                                           fromWhat(e));
                           QFile::remove(cover);
                           return;
                       }

                       try {
                           handleCoverFile(cover, cfg);
                       } catch (const std::exception &e) {
                           errorLogWrapper(sourceUrl,
                                           QStringLiteral("handle cover file"),
                                           fromWhat(e));
                       }
                   });
    wg->wait();

    cleanupDirectory(downloadsDir, cfg);
}

void Application::handleReleaseLink(const Link &link)
{
    const auto cfg = snapshotConfig();
    const QSharedPointer<Beatport> beat = bp;

    QString errText;
    QSharedPointer<Release> release = beat->getRelease(link.id, &errText);
    if (release.isNull()) {
        errorLogWrapper(link.original, QStringLiteral("fetch release"),
                        errText);
        return;
    }

    QString downloadsDir;
    try {
        downloadsDir = setupDownloadsDirectory(
            cfg, cfg->downloadsDirectory,
            release->directoryName(
                namingPrefs(*cfg, cfg->releaseDirectoryTemplate)),
            /*releaseEntity=*/true, release->label.name);
    } catch (const std::exception &e) {
        publishUrlError(link.original,
                        QStringLiteral("setup downloads directory"),
                        fromWhat(e));
        return;
    }

    // 整张专辑共享一个封面：在 downloadSem 内只下载一次。
    QString cover;
    if (requireCover(true, true, cfg)) {
        m_downloadSem->acquire();
        QString coverErr;
        cover = downloadCover(downloadsDir, release->image, cfg, &coverErr);
        m_downloadSem->release();
        if (!coverErr.isEmpty())
            errorLogWrapper(link.original,
                            QStringLiteral("download release cover"), coverErr);
    }

    const auto wg = std::make_shared<WaitGroup>();
    for (const QString &trackUrl : release->trackUrls) {
        downloadWorker(
            wg.get(),
            [this, cfg, beat, release, downloadsDir, cover, trackUrl,
             outerUrl = link.original]() {
                Link trackLink;
                QString errText;
                if (!beat->parseUrl(trackUrl, &trackLink, &errText)) {
                    errorLogWrapper(outerUrl,
                                    QStringLiteral("parse track url"), errText);
                    return;
                }

                QSharedPointer<Track> track =
                    beat->getTrack(trackLink.id, &errText);
                if (track.isNull()) {
                    errorLogWrapper(trackUrl,
                                    QStringLiteral("fetch release track"),
                                    errText);
                    return;
                }
                const QString trackStoreUrl = track->storeUrl();
                track->release = *release;

                try {
                    handleTrack(*track, QString(), downloadsDir, cover, cfg);
                } catch (const std::exception &e) {
                    errorLogWrapper(trackStoreUrl,
                                    QStringLiteral("handle track"),
                                    fromWhat(e));
                }
            });
    }
    wg->wait();

    try {
        handleCoverFile(cover, cfg);
    } catch (const std::exception &e) {
        errorLogWrapper(link.original, QStringLiteral("handle cover file"),
                        fromWhat(e));
        return;
    }

    cleanupDirectory(downloadsDir, cfg);
}

void Application::handlePlaylistLink(const Link &link)
{
    const auto cfg = snapshotConfig();
    const QSharedPointer<Beatport> beat = bp;

    QString errText;
    QSharedPointer<Playlist> playlist = beat->getPlaylist(link.id, &errText);
    if (playlist.isNull()) {
        errorLogWrapper(link.original, QStringLiteral("fetch playlist"),
                        errText);
        return;
    }

    QString downloadsDir;
    try {
        downloadsDir = setupDownloadsDirectory(
            cfg, cfg->downloadsDirectory,
            playlist->directoryName(
                namingPrefs(*cfg, cfg->playlistDirectoryTemplate)),
            /*releaseEntity=*/false, QString());
    } catch (const std::exception &e) {
        publishUrlError(link.original,
                        QStringLiteral("setup downloads directory"),
                        fromWhat(e));
        return;
    }

    const auto wg = std::make_shared<WaitGroup>();

    QString pageErr;
    const bool ok = forPaginated<PlaylistItem>(
        link.id, QString(),
        [beat](qint64 id, int page, const QString &params, QString *e) {
            return beat->getPlaylistItems(id, page, params, e);
        },
        [this, cfg, beat, wg, downloadsDir](const PlaylistItem &src, int) {
            PlaylistItem item = src;
            downloadWorker(
                wg.get(),
                [this, cfg, beat, wg, item, downloadsDir]() mutable {
                    const QString trackStoreUrl = item.track.storeUrl();

                    QString errText;
                    QSharedPointer<Release> release =
                        beat->getRelease(item.track.release.id, &errText);
                    if (release.isNull()) {
                        errorLogWrapper(
                            trackStoreUrl,
                            QStringLiteral("fetch track release"), errText);
                        return;
                    }
                    item.track.release = *release;

                    QString trackDownloadsDir = downloadsDir;
                    QSharedPointer<Track> trackFull =
                        beat->getTrack(item.track.id, &errText);
                    if (trackFull.isNull()) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("fetch full track"),
                                        errText);
                        return;
                    }
                    item.track.number = trackFull->number;

                    if (cfg->sortByContext
                        && cfg->forceReleaseDirectories) {
                        try {
                            trackDownloadsDir = setupDownloadsDirectory(
                                cfg, downloadsDir,
                                release->directoryName(namingPrefs(
                                    *cfg, cfg->releaseDirectoryTemplate)),
                                true, release->label.name);
                        } catch (const std::exception &e) {
                            publishUrlError(
                                trackStoreUrl,
                                QStringLiteral(
                                    "setup track release directory"),
                                fromWhat(e));
                            return;
                        }
                    }

                    QString cover;
                    bool deferRemoveCover = false;
                    if (requireCover(true, cfg->forceReleaseDirectories,
                                     cfg)) {
                        QString coverErr;
                        cover = downloadCover(trackDownloadsDir,
                                              item.track.release.image, cfg,
                                              &coverErr);
                        if (!coverErr.isEmpty()) {
                            errorLogWrapper(
                                trackStoreUrl,
                                QStringLiteral(
                                    "download track release cover"),
                                coverErr);
                        } else if (!cfg->forceReleaseDirectories) {
                            deferRemoveCover = true;
                        }
                    }

                    try {
                        handleTrack(item.track, QString(), trackDownloadsDir,
                                    cover, cfg);
                    } catch (const std::exception &e) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("handle track"),
                                        fromWhat(e));
                        QFile::remove(cover);
                        cleanupDirectory(trackDownloadsDir, cfg);
                        return;
                    }

                    if (cfg->forceReleaseDirectories) {
                        try {
                            handleCoverFile(cover, cfg);
                        } catch (const std::exception &e) {
                            errorLogWrapper(
                                trackStoreUrl,
                                QStringLiteral(
                                    "handle track release cover file"),
                                fromWhat(e));
                            return;
                        }
                    }

                    cleanupDirectory(trackDownloadsDir, cfg);
                    if (deferRemoveCover)
                        QFile::remove(cover);
                });
        },
        &pageErr);

    if (!ok) {
        errorLogWrapper(link.original,
                        QStringLiteral("handle playlist items"), pageErr);
        return;
    }

    wg->wait();
}

void Application::handleChartLink(const Link &link)
{
    const auto cfg = snapshotConfig();
    const QSharedPointer<Beatport> beat = bp;

    QString errText;
    QSharedPointer<Chart> chart = beat->getChart(link.id, &errText);
    if (chart.isNull()) {
        errorLogWrapper(link.original, QStringLiteral("fetch chart"), errText);
        return;
    }

    QString downloadsDir;
    try {
        downloadsDir = setupDownloadsDirectory(
            cfg, cfg->downloadsDirectory,
            chart->directoryName(
                namingPrefs(*cfg, cfg->chartDirectoryTemplate)),
            /*releaseEntity=*/false, QString());
    } catch (const std::exception &e) {
        publishUrlError(link.original,
                        QStringLiteral("setup downloads directory"),
                        fromWhat(e));
        return;
    }

    const auto wg = std::make_shared<WaitGroup>();

    // 排行榜自身封面：requireCover(false,true)，下载后立即 handleCoverFile。
    if (requireCover(false, true, cfg)) {
        downloadWorker(
            wg.get(),
            [this, cfg, wg, chart, downloadsDir,
             outerUrl = link.original]() {
                QString coverErr;
                const QString cover = downloadCover(
                    downloadsDir, chart->image, cfg, &coverErr);
                if (!coverErr.isEmpty())
                    errorLogWrapper(outerUrl,
                                    QStringLiteral("download chart cover"),
                                    coverErr);
                try {
                    handleCoverFile(cover, cfg);
                } catch (const std::exception &e) {
                    errorLogWrapper(outerUrl,
                                    QStringLiteral("handle cover file"),
                                    fromWhat(e));
                }
            });
    }

    QString pageErr;
    const bool ok = forPaginated<Track>(
        link.id, QString(),
        [beat](qint64 id, int page, const QString &params, QString *e) {
            return beat->getChartTracks(id, page, params, e);
        },
        [this, cfg, beat, wg, downloadsDir](const Track &src, int) {
            Track pageTrack = src;
            downloadWorker(
                wg.get(),
                [this, cfg, beat, wg, pageTrack, downloadsDir]() mutable {
                    Track &track = pageTrack;
                    const QString trackStoreUrl = track.storeUrl();

                    QString errText;
                    QSharedPointer<Release> release =
                        beat->getRelease(track.release.id, &errText);
                    if (release.isNull()) {
                        errorLogWrapper(
                            trackStoreUrl,
                            QStringLiteral("fetch track release"), errText);
                        return;
                    }
                    track.release = *release;

                    QString trackDownloadsDir = downloadsDir;
                    QSharedPointer<Track> trackFull =
                        beat->getTrack(track.id, &errText);
                    if (trackFull.isNull()) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("fetch full track"),
                                        errText);
                        return;
                    }
                    track.number = trackFull->number;

                    if (cfg->sortByContext
                        && cfg->forceReleaseDirectories) {
                        try {
                            trackDownloadsDir = setupDownloadsDirectory(
                                cfg, downloadsDir,
                                release->directoryName(namingPrefs(
                                    *cfg, cfg->releaseDirectoryTemplate)),
                                true, release->label.name);
                        } catch (const std::exception &e) {
                            publishUrlError(
                                trackStoreUrl,
                                QStringLiteral(
                                    "setup track release directory"),
                                fromWhat(e));
                            return;
                        }
                    }

                    QString cover;
                    bool deferRemoveCover = false;
                    if (requireCover(true, cfg->forceReleaseDirectories,
                                     cfg)) {
                        QString coverErr;
                        cover = downloadCover(trackDownloadsDir,
                                              track.release.image, cfg,
                                              &coverErr);
                        if (!coverErr.isEmpty()) {
                            errorLogWrapper(
                                trackStoreUrl,
                                QStringLiteral(
                                    "download track release cover"),
                                coverErr);
                        } else if (!cfg->forceReleaseDirectories) {
                            deferRemoveCover = true;
                        }
                    }

                    try {
                        handleTrack(track, QString(), trackDownloadsDir, cover,
                                    cfg);
                    } catch (const std::exception &e) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("handle track"),
                                        fromWhat(e));
                        QFile::remove(cover);
                        cleanupDirectory(trackDownloadsDir, cfg);
                        return;
                    }

                    if (cfg->forceReleaseDirectories) {
                        try {
                            handleCoverFile(cover, cfg);
                        } catch (const std::exception &e) {
                            errorLogWrapper(
                                trackStoreUrl,
                                QStringLiteral(
                                    "handle track release cover file"),
                                fromWhat(e));
                            return;
                        }
                    }

                    cleanupDirectory(trackDownloadsDir, cfg);
                    if (deferRemoveCover)
                        QFile::remove(cover);
                });
        },
        &pageErr);

    // 注意：Go 这里日志文案就是 "handle playlist items"（沿用原样）。
    if (!ok) {
        errorLogWrapper(link.original,
                        QStringLiteral("handle playlist items"), pageErr);
        return;
    }

    wg->wait();
}

void Application::handleLabelLink(const Link &link)
{
    const auto cfg = snapshotConfig();
    const QSharedPointer<Beatport> beat = bp;

    QString errText;
    QSharedPointer<Label> label = beat->getLabel(link.id, &errText);
    if (label.isNull()) {
        errorLogWrapper(link.original, QStringLiteral("fetch label"), errText);
        return;
    }

    QString downloadsDir;
    try {
        downloadsDir = setupDownloadsDirectory(
            cfg, cfg->downloadsDirectory,
            label->directoryName(
                namingPrefs(*cfg, cfg->labelDirectoryTemplate)),
            /*releaseEntity=*/false, QString());
    } catch (const std::exception &e) {
        publishUrlError(link.original,
                        QStringLiteral("setup downloads directory"),
                        fromWhat(e));
        return;
    }

    const auto wg = std::make_shared<WaitGroup>();
    const auto relGroup = std::make_shared<ReleaseSingleFlight>();

    QString pageErr;
    const bool ok = forPaginated<Track>(
        link.id, link.params,
        [beat](qint64 id, int page, const QString &params, QString *e) {
            return beat->getLabelTracks(id, page, params, e);
        },
        [this, cfg, beat, wg, downloadsDir, relGroup](const Track &src, int) {
            Track pageTrack = src;
            downloadWorker(
                wg.get(),
                [this, cfg, beat, wg, pageTrack, downloadsDir,
                 relGroup]() mutable {
                    const QString trackStoreUrl = pageTrack.storeUrl();

                    QString errText;
                    QSharedPointer<Track> track =
                        beat->getTrack(pageTrack.id, &errText);
                    if (track.isNull()) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("fetch track"),
                                        errText);
                        return;
                    }

                    QString releaseErr;
                    QSharedPointer<Release> release =
                        relGroup->call(track->release.id, beat.get(),
                                       &releaseErr);
                    if (release.isNull()) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("fetch track release"),
                                        releaseErr);
                        return;
                    }
                    track->release = *release;

                    QString releaseDir;
                    try {
                        releaseDir = setupDownloadsDirectory(
                            cfg, downloadsDir,
                            release->directoryName(namingPrefs(
                                *cfg, cfg->releaseDirectoryTemplate)),
                            true, release->label.name);
                    } catch (const std::exception &e) {
                        publishUrlError(
                            trackStoreUrl,
                            QStringLiteral(
                                "setup track release downloads directory"),
                            fromWhat(e));
                        return;
                    }

                    // label 无条件下载封面。
                    QString coverErr;
                    const QString cover = downloadCover(
                        releaseDir, release->image, cfg, &coverErr);
                    if (!coverErr.isEmpty())
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral(
                                            "download track release cover"),
                                        coverErr);

                    try {
                        handleTrack(*track, QString(), releaseDir, cover, cfg);
                    } catch (const std::exception &e) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("handle track"),
                                        fromWhat(e));
                        cleanupDirectory(releaseDir, cfg);
                        return;
                    }

                    try {
                        handleCoverFile(cover, cfg);
                    } catch (const std::exception &e) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("handle cover file"),
                                        fromWhat(e));
                        return;
                    }

                    cleanupDirectory(releaseDir, cfg);
                });
        },
        &pageErr);

    if (!ok) {
        errorLogWrapper(link.original,
                        QStringLiteral("handle label releases"), pageErr);
        return;
    }

    wg->wait();
}

void Application::handleArtistLink(const Link &link)
{
    const auto cfg = snapshotConfig();
    const QSharedPointer<Beatport> beat = bp;

    QString errText;
    QSharedPointer<Artist> artist = beat->getArtist(link.id, &errText);
    if (artist.isNull()) {
        errorLogWrapper(link.original, QStringLiteral("fetch artist"), errText);
        return;
    }

    QString downloadsDir;
    try {
        downloadsDir = setupDownloadsDirectory(
            cfg, cfg->downloadsDirectory,
            artistDirectoryName(*artist,
                                namingPrefs(*cfg,
                                           cfg->artistDirectoryTemplate)),
            /*releaseEntity=*/false, QString());
    } catch (const std::exception &e) {
        publishUrlError(link.original,
                        QStringLiteral("setup downloads directory"),
                        fromWhat(e));
        return;
    }

    const auto wg = std::make_shared<WaitGroup>();
    const auto relGroup = std::make_shared<ReleaseSingleFlight>();

    QString pageErr;
    const bool ok = forPaginated<Track>(
        link.id, link.params,
        [beat](qint64 id, int page, const QString &params, QString *e) {
            return beat->getArtistTracks(id, page, params, e);
        },
        [this, cfg, beat, wg, downloadsDir, relGroup](const Track &src, int) {
            Track pageTrack = src;
            downloadWorker(
                wg.get(),
                [this, cfg, beat, wg, pageTrack, downloadsDir,
                 relGroup]() mutable {
                    const QString trackStoreUrl = pageTrack.storeUrl();

                    QString errText;
                    QSharedPointer<Track> track =
                        beat->getTrack(pageTrack.id, &errText);
                    if (track.isNull()) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("fetch full track"),
                                        errText);
                        return;
                    }

                    QString releaseErr;
                    QSharedPointer<Release> release =
                        relGroup->call(track->release.id, beat.get(),
                                       &releaseErr);
                    if (release.isNull()) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("fetch track release"),
                                        releaseErr);
                        return;
                    }
                    track->release = *release;

                    QString releaseDir;
                    try {
                        releaseDir = setupDownloadsDirectory(
                            cfg, downloadsDir,
                            release->directoryName(namingPrefs(
                                *cfg, cfg->releaseDirectoryTemplate)),
                            true, release->label.name);
                    } catch (const std::exception &e) {
                        publishUrlError(
                            trackStoreUrl,
                            QStringLiteral(
                                "setup track release downloads directory"),
                            fromWhat(e));
                        return;
                    }

                    QString cover;
                    if (requireCover(true, true, cfg)) {
                        QString coverErr;
                        cover = downloadCover(releaseDir, release->image, cfg,
                                              &coverErr);
                        if (!coverErr.isEmpty())
                            errorLogWrapper(trackStoreUrl,
                                            QStringLiteral(
                                                "download track release cover"),
                                            coverErr);
                    }

                    try {
                        handleTrack(*track, QString(), releaseDir, cover, cfg);
                    } catch (const std::exception &e) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("handle track"),
                                        fromWhat(e));
                        QFile::remove(cover);
                        cleanupDirectory(releaseDir, cfg);
                        return;
                    }

                    try {
                        handleCoverFile(cover, cfg);
                    } catch (const std::exception &e) {
                        errorLogWrapper(trackStoreUrl,
                                        QStringLiteral("handle cover file"),
                                        fromWhat(e));
                        return;
                    }

                    cleanupDirectory(releaseDir, cfg);
                });
        },
        &pageErr);

    if (!ok) {
        errorLogWrapper(link.original,
                        QStringLiteral("handle artist tracks"), pageErr);
        return;
    }

    wg->wait();
}
