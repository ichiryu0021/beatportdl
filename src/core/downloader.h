#pragma once

#include "config.h"
#include "models.h"
#include "beatport.h"
#include "auth.h"
#include "integrity.h"
#include "archiver.h"
#include "util.h"

#include <QAtomicInt>
#include <QJsonObject>
#include <QRecursiveMutex>
#include <QSemaphore>
#include <QSet>
#include <QStringList>
#include <QSharedPointer>
#include <functional>
#include <memory>
#include <thread>

// RPC 业务错误（对应 Go server 方法返回的 error，统一 code -32000）。
struct RpcException {
    int code = -32000;
    QString message;
};

// SSE 事件出口（rpcserver 实现；CLI 模式为 nullptr）。
class EventSink {
public:
    virtual ~EventSink() = default;
    virtual void publishEvent(const QJsonObject &event) = 0;
};

class QFile;

class Application {
public:
    ~Application();

    // 对应 main.go Setup：
    // serveMode=true  → ParseLenient（凭据缺失允许延迟认证）
    // cliInteractive=true → 配置文件不存在时交互式创建
    static Application *setup(const QString &explicitConfigPath,
                              bool serveMode, bool cliInteractive, QString *err);

    void setEventSink(EventSink *sink) { m_sink = sink; }

    // ---------- server 适配层（方法/错误文本严格对齐 internal/server/app.go） ----------
    bool isAuthReady();
    void login(const QString &username, const QString &password); // 成功后重建 auth/bp
    void logout();

    struct DirStatus {
        QString path;
        bool writable = false;
        QString error;
    };
    DirStatus probeDownloadsDirectory(const QString &path) const;
    void setDownloadsDirectory(const QString &rawPath);

    struct SettingsView {
        QString downloadsDirectory;
        bool writable = false;
        QString dirError;
        QString quality;
        QString trackExists;
        QString trackFileTemplate;
        QString proxyMode; // off / auto / manual
        QString proxyUrl;
        QString theme; // light / dark
        QString language; // system / en / zh
        bool sortByGenre = false;
        QString genreSortMode; // genre / subgenre_or_genre
    };
    SettingsView getSettings();

    void setQuality(const QString &quality);
    void setTrackExists(const QString &mode);
    void setTrackFileTemplate(const QString &rawTemplate);
    // 代理热切换：mode=off/auto/manual；manual 时 url 必填（http(s)/socks5）。
    // 写盘后更新全局 ProxySetting，所有出网通道下一次请求即生效。
    void setProxy(const QString &mode, const QString &url);

    // GUI 主题切换：mode=light/dark，写盘持久化。
    void setTheme(const QString &mode);

    // GUI 语言切换：mode=system/en/zh，写盘持久化（重启后生效）。
    void setLanguage(const QString &mode);

    // 曲风子文件夹开关：enabled=false 关闭；true 时 mode 取
    // genre / subgenre_or_genre。写盘并立即重建 Archiver。
    void setGenreSorting(bool enabled, const QString &mode);

    // 入队去重（pending/active/本次三方），重复的发 download.skipped/reason=duplicate。
    // 返回 (enqueued, skipped)。
    QPair<int, int> enqueueURLs(const QStringList &urls);

    // ---------- 服务主循环 / 处理 ----------
    // 阻塞循环：250ms 轮询 pending → 非空则每 URL 起 globalWorker 跑 handleUrl，等完一批。
    void serveLoop();
    // 立即跑一批（CLI 模式）：drain→workers→wait。
    void runOnce();
    void handleUrl(const QString &url);

    // 停止信号（worker 在获取 downloadSem 前检查）。
    std::atomic<bool> stopFlag{false};
    bool useConsoleProgress = false;

    QSharedPointer<Beatport> bp;
    std::shared_ptr<Auth> auth;

    // ---------- 供 rpcserver 构造调度器时取认证状态 ----------
    void rebuildArchiver(); // sort_by_genre 开关或目录变化后调用

private:
    Application();

    using ConfigPtr = std::shared_ptr<AppConfig>;

    // 配置快照（Qt COW 共享，setter 在锁内替换指针）。
    ConfigPtr snapshotConfig() const;
    std::shared_ptr<Archiver> snapshotArchiver();
    QString saveConfigLocked(); // 持久化（configSaveMu）；空串=成功

    // worker 池（globalSem=max_global_workers, downloadSem=max_download_workers）
    void globalWorker(WaitGroup *wg, const std::function<void()> &fn);
    void downloadWorker(WaitGroup *wg, const std::function<void()> &fn);

    // 六种链接处理器
    void handleTrackLink(const Link &link, const QString &sourceUrl);
    void handleReleaseLink(const Link &link);
    void handlePlaylistLink(const Link &link);
    void handleChartLink(const Link &link);
    void handleLabelLink(const Link &link);
    void handleArtistLink(const Link &link);

    // 单曲流水线（cfg 为入口快照，全程沿用）
    void handleTrack(const Track &track, const QString &sourceUrl,
                     const QString &dir, const QString &coverPath,
                     const ConfigPtr &cfg);
    QString saveTrack(const Track &track, const QString &dir,
                      const QString &eventURL, const ConfigPtr &cfg);
    bool downloadFile(const QString &url, const QString &destination,
                      const QString &progressPrefix);
    void tagTrack(const QString &path, const Track &track,
                  const QString &coverPath, const ConfigPtr &cfg);

    // sortByContext 时把 baseDir 套上实体子目录（release 可再套 label）。
    // baseDir 对应 Go setupDownloadsDirectory 的第一参：顶层传配置下载目录，
    // playlist/chart/label/artist 的强制 release 子目录传上层实体目录。
    QString setupDownloadsDirectory(const ConfigPtr &cfg, const QString &baseDir,
                                    const QString &subDir, bool releaseEntity,
                                    const QString &labelName);
    void cleanupDirectory(const QString &dir, const ConfigPtr &cfg);
    QString downloadCover(const QString &dir, const Image &image,
                          const ConfigPtr &cfg, QString *err);
    void handleCoverFile(const QString &path, const ConfigPtr &cfg);
    bool requireCover(bool respectFixTags, bool respectKeepCover,
                      const ConfigPtr &cfg);

    // 事件 / 日志
    void publishEvent(const QJsonObject &ev);
    void publishDownloadEvent(const QString &event, const QString &url,
                              const QString &title, const QJsonObject &extra = {});
    void publishUrlError(const QString &url, const QString &step, const QString &errText);
    void logErrorEx(const QString &caller, const QString &errText);
    void errorLogWrapper(const QString &url, const QString &step, const QString &errText);
    void infoLogWrapper(const QString &url, const QString &message);
    void appendErrorLogLine(const QString &line);

    // URL 队列（适配层）
    QStringList drainPending();
    void resetActiveURLs();

    // 活动文件名冲突表
    bool registerActiveFile(const QString &path);   // 已存在返回 false
    void unregisterActiveFile(const QString &path);
    QString uniqueActivePath(const QString &path) const;

    QString m_configPath;
    mutable QRecursiveMutex m_settingsMu;
    ConfigPtr m_config;
    std::shared_ptr<Archiver> m_archiver;
    QMutex m_archiverMu;
    QMutex m_configSaveMu;

    std::unique_ptr<Checker> m_checker;
    EventSink *m_sink = nullptr;

    // Setup 时按配置 max*Workers 重建（Go semaphore.Weighted）。
    std::unique_ptr<QSemaphore> m_globalSem;
    std::unique_ptr<QSemaphore> m_downloadSem;

    // write_error_log=true 时打开的 exe 旁 bpdownloader-err.log（Go app.logFile）。
    std::unique_ptr<QFile> m_errorLog;
    QMutex m_errorLogMu;

    QMutex m_urlsMu;
    QStringList m_pendingURLs;
    QStringList m_activeURLs;

    mutable QRecursiveMutex m_authMu;
    bool m_authReady = false;

    mutable QMutex m_activeFilesMu;
    QSet<QString> m_activeFiles;
};
