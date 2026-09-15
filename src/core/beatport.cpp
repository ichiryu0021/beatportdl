#include "beatport.h"

#include "genres_scrape.h"
#include "util.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>
#include <QMap>
#include <QNetworkCookie>
#include <QString>
#include <QUrl>

#include <cstring>

namespace {

constexpr qint64 kGenrePageLimitBytes = 8 << 20; // io.LimitReader(8<<20)

// application/x-www-form-urlencoded：url.Values.Encode() 按 key 排序、
// QueryEscape 键值后以 '&' 连接（QMap 天然按 key 有序）。
QByteArray encodeForm(const QMap<QString, QString> &form)
{
    QByteArray out;
    for (auto it = form.constBegin(); it != form.constEnd(); ++it) {
        if (!out.isEmpty())
            out += '&';
        out += queryEscape(it.key());
        out += '=';
        out += queryEscape(it.value());
    }
    return out;
}

bool parseJson(const QByteArray &body, QJsonDocument *doc, QString *err)
{
    QJsonParseError pe;
    *doc = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError) {
        if (err)
            *err = pe.errorString();
        return false;
    }
    return true;
}

// Qt 不暴露 HTTP reason phrase；按 Go net/http 状态文案补一份常用表，
// 用于 "genre page status: <code> <reason>"。
QString reasonPhrase(int status)
{
    switch (status) {
    case 100: return QStringLiteral("Continue");
    case 200: return QStringLiteral("OK");
    case 201: return QStringLiteral("Created");
    case 204: return QStringLiteral("No Content");
    case 301: return QStringLiteral("Moved Permanently");
    case 302: return QStringLiteral("Found");
    case 303: return QStringLiteral("See Other");
    case 304: return QStringLiteral("Not Modified");
    case 307: return QStringLiteral("Temporary Redirect");
    case 308: return QStringLiteral("Permanent Redirect");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 408: return QStringLiteral("Request Timeout");
    case 409: return QStringLiteral("Conflict");
    case 410: return QStringLiteral("Gone");
    case 429: return QStringLiteral("Too Many Requests");
    case 500: return QStringLiteral("Internal Server Error");
    case 501: return QStringLiteral("Not Implemented");
    case 502: return QStringLiteral("Bad Gateway");
    case 503: return QStringLiteral("Service Unavailable");
    case 504: return QStringLiteral("Gateway Timeout");
    default:  return QStringLiteral("Error");
    }
}

} // namespace

QSharedPointer<Beatport> Beatport::create(std::shared_ptr<Auth> auth,
                                          QString *err)
{
    QSharedPointer<Beatport> b(new Beatport());
    b->m_auth = std::move(auth);
    // 代理策略由全局 ProxySetting 决定（setGlobalProxy），m_http 不再单独持有。
    // 与 Go New() 的浏览器请求头逐字一致。
    b->m_headers = {
        {QByteArray("accept"),
         QByteArray("text/html,application/xhtml+xml,application/xml;q=0.9,"
                    "image/avif,image/webp,image/apng,*/*;q=0.8,"
                    "application/signed-exchange;v=b3;q=0.7")},
        {QByteArray("accept-language"), QByteArray("en-US,en;q=0.9")},
        {QByteArray("cache-control"), QByteArray("max-age=0")},
        {QByteArray("user-agent"),
         QByteArray("Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                    "AppleWebKit/537.36 (KHTML, like Gecko) "
                    "Chrome/123.0.0.0 Safari/537.36")},
    };
    if (err)
        err->clear();
    return b;
}

void Beatport::setAuth(std::shared_ptr<Auth> auth)
{
    m_auth = std::move(auth);
}

bool Beatport::fetchInternal(const QByteArray &method, const QString &endpoint,
                             const QByteArray &payload,
                             const QByteArray &contentType,
                             QByteArray *bodyOut, QByteArray *locationOut,
                             QList<QNetworkCookie> *cookiesOut, QString *err)
{
    const bool isAuthEndpoint = endpoint == kTokenEndpoint
                                || endpoint == kAuthorizeEndpoint
                                || endpoint == kLoginEndpoint;

    // 非 auth 端点先 Auth.Check()。
    if (!isAuthEndpoint) {
        if (!m_auth) {
            if (err)
                *err = QStringLiteral("not authenticated: no cached token");
            return false;
        }
        QString checkErr;
        if (!m_auth->check(this, &checkErr)) {
            if (err)
                *err = checkErr;
            return false;
        }
    }

    const QUrl url(m_baseUrl + endpoint, QUrl::TolerantMode);

    QMap<QByteArray, QByteArray> headers = m_headers;
    if (m_auth && m_auth->hasToken()) {
        headers.insert(QByteArray("authorization"),
                       QByteArray("Bearer ") + m_auth->accessToken().toUtf8());
    }

    // BlockingClient 不跟随 302（ManualRedirectPolicy），与 Go
    // CheckRedirect: ErrUseLastResponse 一致。
    const HttpResult res = m_http.request(method, url, headers, payload,
                                          contentType, false);

    if (!res.networkError.isEmpty()) {
        if (err)
            *err = QStringLiteral("request failed: %1").arg(res.networkError);
        return false;
    }

    if (res.status != 200 && res.status != 302) {
        // 401（排除三个 auth 端点）→ Invalidate 后单层递归重试一次。
        if (res.status == 401 && !isAuthEndpoint && !m_fetchInFlight) {
            m_auth->invalidate();
            m_fetchInFlight = true;
            const bool ok = fetchInternal(method, endpoint, payload,
                                          contentType, bodyOut, locationOut,
                                          cookiesOut, err);
            m_fetchInFlight = false;
            return ok;
        }

        // 错误体可能是 {"detail"/"error": "..."}（对象）、"纯 JSON 字符串"
        // （如登录 403 的 "Incorrect username or password."）或非 JSON 文本。
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(res.body, &pe);
        QString detail;
        if (pe.error == QJsonParseError::NoError && doc.isObject()) {
            const QJsonObject o = doc.object();
            const QJsonValue detailVal = o.value(QStringLiteral("detail"));
            const QJsonValue errorVal = o.value(QStringLiteral("error"));
            if (detailVal.isString())
                detail = detailVal.toString();
            else if (errorVal.isString())
                detail = errorVal.toString();
        }
        if (detail.isEmpty()) {
            // 顶层 JSON 字符串（QJsonDocument 只接受对象/数组）：包一层
            // 数组再解析，如 "Incorrect username or password."。
            const QJsonDocument wrap =
                QJsonDocument::fromJson("[" + res.body + "]");
            if (wrap.isArray()) {
                const QJsonArray a = wrap.array();
                if (a.size() == 1 && a.first().isString())
                    detail = a.first().toString();
            }
        }
        if (detail.isEmpty())
            detail = QString::fromUtf8(res.body).trimmed();
        if (detail.isEmpty())
            detail = QStringLiteral("Unknown error");
        if (detail.length() > 300)
            detail = detail.left(300);
        if (err) {
            *err = QStringLiteral("request failed with status code: %1 - %2")
                       .arg(res.status)
                       .arg(detail);
        }
        return false;
    }

    if (bodyOut)
        *bodyOut = res.body;
    if (locationOut)
        *locationOut = res.header(QByteArray("location"));
    if (cookiesOut)
        *cookiesOut = res.cookies;
    return true;
}

bool Beatport::fetchGet(const QString &endpoint, QByteArray *bodyOut,
                        QString *err)
{
    return fetchInternal(QByteArray("GET"), endpoint, QByteArray(),
                         QByteArray(), bodyOut, nullptr, nullptr, err);
}

bool Beatport::fetchJson(const QString &method, const QString &endpoint,
                         const QJsonObject &payload, QByteArray *bodyOut,
                         QByteArray *locationOut,
                         QList<QNetworkCookie> *cookiesOut, QString *err)
{
    // json.Encoder.Encode 末尾带 '\n'。
    QByteArray body = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    body += '\n';
    return fetchInternal(method.toUtf8(), endpoint, body,
                         QByteArray("application/json"), bodyOut, locationOut,
                         cookiesOut, err);
}

bool Beatport::fetchForm(const QString &method, const QString &endpoint,
                         const QMap<QString, QString> &payload,
                         QByteArray *bodyOut, QString *err)
{
    const QByteArray body = encodeForm(payload);
    return fetchInternal(method.toUtf8(), endpoint, body,
                         QByteArray("application/x-www-form-urlencoded"),
                         bodyOut, nullptr, nullptr, err);
}

// ---------------------------------------------------------------------------
// catalog API
// ---------------------------------------------------------------------------

QSharedPointer<Track> Beatport::getTrack(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/tracks/%1/").arg(id), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<Track>(new Track(parseTrack(doc.object())));
}

QSharedPointer<TrackDownload> Beatport::downloadTrack(qint64 id,
                                                      const QString &quality,
                                                      QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/tracks/%1/download/?quality=%2")
            .arg(id)
            .arg(QString::fromUtf8(queryEscape(quality)));
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<TrackDownload>(
        new TrackDownload(parseTrackDownload(doc.object())));
}

QSharedPointer<TrackStream> Beatport::streamTrack(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/tracks/%1/stream/").arg(id), &body,
                  err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<TrackStream>(
        new TrackStream(parseTrackStream(doc.object())));
}

QSharedPointer<Release> Beatport::getRelease(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/releases/%1/").arg(id), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<Release>(new Release(parseRelease(doc.object())));
}

QSharedPointer<PaginatedTracks> Beatport::getReleaseTracks(
    qint64 id, int page, const QString &params, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/releases/%1/tracks/?page=%2&%3")
            .arg(id)
            .arg(page)
            .arg(params);
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedTracks>(
        new PaginatedTracks(parsePaginatedTracks(doc.object())));
}

QSharedPointer<Artist> Beatport::getArtist(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/artists/%1/").arg(id), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<Artist>(new Artist(parseArtist(doc.object())));
}

QSharedPointer<PaginatedTracks> Beatport::getArtistTracks(
    qint64 id, int page, const QString &params, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/tracks/?page=%1&artist_id=%2&%3")
            .arg(page)
            .arg(id)
            .arg(params);
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedTracks>(
        new PaginatedTracks(parsePaginatedTracks(doc.object())));
}

QSharedPointer<Label> Beatport::getLabel(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/labels/%1/").arg(id), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<Label>(new Label(parseLabel(doc.object())));
}

QSharedPointer<PaginatedReleases> Beatport::getLabelReleases(
    qint64 id, int page, const QString &params, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/labels/%1/releases/?page=%2&%3")
            .arg(id)
            .arg(page)
            .arg(params);
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedReleases>(
        new PaginatedReleases(parsePaginatedReleases(doc.object())));
}

QSharedPointer<PaginatedTracks> Beatport::getLabelTracks(
    qint64 id, int page, const QString &params, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/tracks/?page=%1&label_id=%2&%3")
            .arg(page)
            .arg(id)
            .arg(params);
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedTracks>(
        new PaginatedTracks(parsePaginatedTracks(doc.object())));
}

QSharedPointer<Playlist> Beatport::getPlaylist(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/playlists/%1/").arg(id), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<Playlist>(new Playlist(parsePlaylist(doc.object())));
}

QSharedPointer<PaginatedPlaylistItems> Beatport::getPlaylistItems(
    qint64 id, int page, const QString &params, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/playlists/%1/tracks/?page=%2&%3")
            .arg(id)
            .arg(page)
            .arg(params);
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedPlaylistItems>(
        new PaginatedPlaylistItems(parsePaginatedPlaylistItems(doc.object())));
}

QSharedPointer<Chart> Beatport::getChart(qint64 id, QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/charts/%1/").arg(id), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<Chart>(new Chart(parseChart(doc.object())));
}

QSharedPointer<PaginatedTracks> Beatport::getChartTracks(
    qint64 id, int page, const QString &params, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/charts/%1/tracks/?page=%2&%3")
            .arg(id)
            .arg(page)
            .arg(params);
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedTracks>(
        new PaginatedTracks(parsePaginatedTracks(doc.object())));
}

QString buildLatestParams(const QString &genreName, int page, int perPage,
                          const QString &dateFrom, const QString &dateTo)
{
    QString params =
        QStringLiteral("page=%1&per_page=%2&is_available_for_streaming=true")
            .arg(page)
            .arg(perPage);
    if (!genreName.isEmpty()) {
        params += QStringLiteral("&genre_name=")
                  + QString::fromUtf8(queryEscape(genreName));
    }
    if (!dateFrom.isEmpty() || !dateTo.isEmpty())
        params += QStringLiteral("&new_release_date=%1:%2").arg(dateFrom, dateTo);
    return params;
}

QSharedPointer<PaginatedTracks> Beatport::getLatestTracks(
    const QString &genreName, int page, int perPage, const QString &dateFrom,
    const QString &dateTo, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/tracks/?order_by=-publish_date&%1")
            .arg(buildLatestParams(genreName, page, perPage, dateFrom, dateTo));
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedTracks>(
        new PaginatedTracks(parsePaginatedTracks(doc.object())));
}

QSharedPointer<PaginatedReleases> Beatport::getLatestReleases(
    const QString &genreName, int page, int perPage, const QString &dateFrom,
    const QString &dateTo, QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/releases/?order_by=-new_release_date&%1")
            .arg(buildLatestParams(genreName, page, perPage, dateFrom, dateTo));
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedReleases>(
        new PaginatedReleases(parsePaginatedReleases(doc.object())));
}

QSharedPointer<PaginatedGenres> Beatport::getGenres(QString *err)
{
    QByteArray body;
    if (!fetchGet(QStringLiteral("/catalog/genres/"), &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<PaginatedGenres>(
        new PaginatedGenres(parsePaginatedGenres(doc.object())));
}

QSharedPointer<SearchResults> Beatport::search(const QString &query,
                                               QString *err)
{
    const QString endpoint =
        QStringLiteral("/catalog/search/?q=%1&order_by=-publish_date"
                       "&is_available_for_streaming=true")
            .arg(QString::fromUtf8(queryEscape(query)));
    QByteArray body;
    if (!fetchGet(endpoint, &body, err))
        return nullptr;
    QJsonDocument doc;
    if (!parseJson(body, &doc, err))
        return nullptr;
    return QSharedPointer<SearchResults>(
        new SearchResults(parseSearchResults(doc.object())));
}

QList<Genre> Beatport::scrapeGenres(QString *err)
{
    // 与 Go ScrapeGenres 相同：使用配置代理的 API 客户端（不跟随重定向），
    // 仅带 user-agent / accept / accept-language 三个头。
    QMap<QByteArray, QByteArray> headers;
    headers.insert(QByteArray("user-agent"),
                   m_headers.value(QByteArray("user-agent")));
    headers.insert(QByteArray("accept"),
                   m_headers.value(QByteArray("accept")));
    headers.insert(QByteArray("accept-language"),
                   m_headers.value(QByteArray("accept-language")));

    const HttpResult res = m_http.request(
        QByteArray("GET"),
        QUrl(QStringLiteral("https://www.beatport.com/")),
        headers, QByteArray(), QByteArray(), false);

    if (!res.networkError.isEmpty()) {
        if (err)
            *err = QStringLiteral("fetch genre page: %1")
                       .arg(res.networkError);
        return {};
    }
    if (res.status != 200) {
        if (err)
            *err = QStringLiteral("genre page status: %1 %2")
                       .arg(res.status)
                       .arg(reasonPhrase(res.status));
        return {};
    }

    // io.LimitReader：最多解析前 8MiB（BlockingClient 已读全量，这里截断到
    // 同样的字节窗口；正则不会跨越窗口产生 Go 中也不存在的匹配）。
    const QByteArray window = res.body.size() > kGenrePageLimitBytes
                                  ? res.body.left(int(kGenrePageLimitBytes))
                                  : res.body;
    return parseGenresHtml(QString::fromUtf8(window), err);
}

bool Beatport::parseUrl(const QString &inputUrl, Link *out, QString *err)
{
    return parseBeatportUrl(inputUrl, out, err);
}
