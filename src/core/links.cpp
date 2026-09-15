#include "links.h"

#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

QString linkTypeName(LinkType t)
{
    switch (t) {
    case LinkType::Track:
        return QStringLiteral("tracks");
    case LinkType::Release:
        return QStringLiteral("releases");
    case LinkType::Playlist:
        return QStringLiteral("playlists");
    case LinkType::Chart:
        return QStringLiteral("charts");
    case LinkType::Label:
        return QStringLiteral("labels");
    case LinkType::Artist:
        return QStringLiteral("artists");
    }
    return {};
}

// 对应 Go strconv.ParseInt 失败时的错误文本：
// strconv.ParseInt: parsing "X": invalid syntax / value out of range
static bool parseGoInt64(const QString &seg, qint64 *out, QString *err)
{
    bool ok = false;
    const qint64 v = seg.toLongLong(&ok, 10);
    if (ok) {
        *out = v;
        return true;
    }
    static const QRegularExpression numeric(QStringLiteral("^[+-]?[0-9]+$"));
    const QString reason = numeric.match(seg).hasMatch()
                               ? QStringLiteral("value out of range")
                               : QStringLiteral("invalid syntax");
    if (err) {
        *err = QStringLiteral("invalid id: strconv.ParseInt: parsing \"%1\": %2")
                   .arg(seg, reason);
    }
    return false;
}

bool parseBeatportUrl(const QString &inputUrl, Link *out, QString *err)
{
    // 对应 Go url.Parse（容忍模式）；解析失败时 Go 直接返回 url 错误。
    const QUrl url(inputUrl, QUrl::TolerantMode);
    if (!url.isValid()) {
        if (err)
            *err = url.errorString();
        return false;
    }

    // strings.Trim(u.Path, "/") 后按 '/' Split（保留空段）。
    // QUrl::path() 默认 FullyDecoded，等价 Go u.Path。
    QString path = url.path();
    while (path.startsWith(QLatin1Char('/')))
        path = path.mid(1);
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    QStringList segments = path.split(QLatin1Char('/'), Qt::KeepEmptyParts);
    int segmentsLength = segments.size();

    Link link;
    link.original = inputUrl;

    if (url.host() != QLatin1String("www.beatport.com")
        && url.host() != QLatin1String("api.beatport.com")) {
        if (err)
            *err = QStringLiteral("invalid url");
        return false;
    }

    if (segmentsLength == 0) {
        if (err)
            *err = QStringLiteral("invalid url");
        return false;
    }

    if (segmentsLength > 1 && segments.at(0).size() == 2) {
        segments.removeFirst();
        --segmentsLength;

        if (segments.at(0) == QLatin1String("catalog")) {
            segments.removeFirst();
            --segmentsLength;
        }
    }

    // Go 的 idSegment 是段数组下标（0 基）。
    int idSegment = -1;
    const QString &head = segments.at(0);
    if (head == QLatin1String("track")) {
        idSegment = 2;
        link.type = LinkType::Track;
    } else if (head == QLatin1String("release")) {
        idSegment = 2;
        link.type = LinkType::Release;
    } else if (head == QLatin1String("library")) {
        // Go 在此直接取 segments[1]，段不足会 panic；此处以 invalid url 兜底。
        if (segmentsLength < 2) {
            if (err)
                *err = QStringLiteral("invalid url");
            return false;
        }
        if (segments.at(1) == QLatin1String("playlists")
            || segments.at(1) == QLatin1String("playlist")) {
            idSegment = 2;
            link.type = LinkType::Playlist;
        } else {
            if (err) {
                *err = QStringLiteral("invalid link type: %1/%2")
                           .arg(segments.at(0), segments.at(1));
            }
            return false;
        }
    } else if (head == QLatin1String("playlists")) {
        idSegment = 2;
        link.type = LinkType::Playlist;
    } else if (head == QLatin1String("chart") || head == QLatin1String("playlist")) {
        idSegment = 2;
        link.type = LinkType::Chart;
    } else if (head == QLatin1String("label")) {
        idSegment = 2;
        link.type = LinkType::Label;
    } else if (head == QLatin1String("artist")) {
        idSegment = 2;
        link.type = LinkType::Artist;
    } else if (head == QLatin1String("tracks")) {
        idSegment = 1;
        link.type = LinkType::Track;
    } else if (head == QLatin1String("releases")) {
        idSegment = 1;
        link.type = LinkType::Release;
    } else {
        if (err)
            *err = QStringLiteral("invalid url");
        return false;
    }

    if (idSegment + 1 > segmentsLength) {
        if (err)
            *err = QStringLiteral("invalid url");
        return false;
    }

    qint64 id = 0;
    QString idErr;
    if (!parseGoInt64(segments.at(idSegment), &id, &idErr)) {
        if (err)
            *err = idErr;
        return false;
    }
    link.id = id;

    // 对应 u.RawQuery（保留百分号编码的原始查询串）。
    link.params = url.query(QUrl::FullyEncoded);

    if (out)
        *out = std::move(link);
    return true;
}
