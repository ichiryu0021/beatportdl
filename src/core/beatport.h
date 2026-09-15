#pragma once

#include "models.h"
#include "links.h"
#include "http.h"
#include "auth.h"

#include <QMap>
#include <QSharedPointer>
#include <QJsonObject>

class Beatport {
public:
    static QSharedPointer<Beatport> create(std::shared_ptr<Auth> auth,
                                           QString *err);

    void setAuth(std::shared_ptr<Auth> auth);
    std::shared_ptr<Auth> getAuth() const { return m_auth; }

    // 通用 fetch（对应 Go Beatport.fetch）：
    // - 非 auth 端点先 auth->Check()
    // - JSON/form 编码，Bearer token，浏览器请求头
    // - 仅接受 200/302；401（非 auth 端点）Invalidate 后递归重试一次
    // - 其他错误解码 {detail}/{error}，消息 "request failed with status code: N - detail"
    bool fetchGet(const QString &endpoint, QByteArray *bodyOut, QString *err);
    bool fetchJson(const QString &method, const QString &endpoint,
                   const QJsonObject &payload, QByteArray *bodyOut,
                   QByteArray *locationOut, QList<QNetworkCookie> *cookiesOut,
                   QString *err);
    bool fetchForm(const QString &method, const QString &endpoint,
                   const QMap<QString, QString> &payload, QByteArray *bodyOut,
                   QString *err);

    // catalog API
    QSharedPointer<Track> getTrack(qint64 id, QString *err);
    QSharedPointer<TrackDownload> downloadTrack(qint64 id, const QString &quality, QString *err);
    QSharedPointer<TrackStream> streamTrack(qint64 id, QString *err);

    QSharedPointer<Release> getRelease(qint64 id, QString *err);
    QSharedPointer<PaginatedTracks> getReleaseTracks(qint64 id, int page,
                                                     const QString &params, QString *err);

    QSharedPointer<Artist> getArtist(qint64 id, QString *err);
    QSharedPointer<PaginatedTracks> getArtistTracks(qint64 id, int page,
                                                    const QString &params, QString *err);

    QSharedPointer<Label> getLabel(qint64 id, QString *err);
    QSharedPointer<PaginatedReleases> getLabelReleases(qint64 id, int page,
                                                       const QString &params, QString *err);
    QSharedPointer<PaginatedTracks> getLabelTracks(qint64 id, int page,
                                                   const QString &params, QString *err);

    QSharedPointer<Playlist> getPlaylist(qint64 id, QString *err);
    QSharedPointer<PaginatedPlaylistItems> getPlaylistItems(qint64 id, int page,
                                                            const QString &params, QString *err);

    QSharedPointer<Chart> getChart(qint64 id, QString *err);
    QSharedPointer<PaginatedTracks> getChartTracks(qint64 id, int page,
                                                   const QString &params, QString *err);

    QSharedPointer<PaginatedTracks> getLatestTracks(const QString &genreName, int page,
                                                    int perPage, const QString &dateFrom,
                                                    const QString &dateTo, QString *err);
    QSharedPointer<PaginatedReleases> getLatestReleases(const QString &genreName, int page,
                                                        int perPage, const QString &dateFrom,
                                                        const QString &dateTo, QString *err);
    QSharedPointer<PaginatedGenres> getGenres(QString *err);
    QSharedPointer<SearchResults> search(const QString &query, QString *err);

    // 官网首页曲风抓取（用配置代理的 API 客户端、不跟随重定向、仅取 8MB）。
    QList<Genre> scrapeGenres(QString *err);

    // ParseUrl（无状态成员，转发给 parseBeatportUrl）。
    bool parseUrl(const QString &inputUrl, Link *out, QString *err);

    BlockingClient &httpClient() { return m_http; }

private:
    Beatport() = default;

    // 三个公有 fetch* 的统一内核（对应 Go Beatport.fetch 唯一入口）。
    // contentType 为空表示无 payload（不发 body / Content-Type）；
    // Auth（友元）的 authorize GET 也走这里。
    bool fetchInternal(const QByteArray &method, const QString &endpoint,
                       const QByteArray &payload, const QByteArray &contentType,
                       QByteArray *bodyOut, QByteArray *locationOut,
                       QList<QNetworkCookie> *cookiesOut, QString *err);

    QString m_baseUrl = QStringLiteral("https://api.beatport.com/v4");
    QMap<QByteArray, QByteArray> m_headers;
    std::shared_ptr<Auth> m_auth;
    BlockingClient m_http{40000};
    bool m_fetchInFlight = false; // 401 递归保护（等价 Go 单层递归）

    friend struct Auth;
};

// 构造 /catalog/tracks|releases 查询参数（与 BuildLatestParams 一致）。
QString buildLatestParams(const QString &genreName, int page, int perPage,
                          const QString &dateFrom, const QString &dateTo);
