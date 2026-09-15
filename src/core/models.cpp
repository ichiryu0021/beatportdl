#include "models.h"
#include "util.h"

#include <QJsonArray>
#include <QRegularExpression>

namespace {

// QJsonDocument 已把 JSON 转义（\n 等）解码为真实控制字符；删除它们再做
// 空白压缩，与 Go SanitizedString 原始文本处理结果一致。
QString sanitized(const QJsonValue &v)
{
    QString s = v.toString();
    s.remove(QLatin1Char('\n'));
    s.remove(QLatin1Char('\r'));
    s.remove(QLatin1Char('\t'));
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    s.replace(ws, QStringLiteral(" "));
    return s.trimmed();
}

QString str(const QJsonObject &o, const char *key)
{
    return o.value(QLatin1String(key)).toString();
}

int integer(const QJsonObject &o, const char *key)
{
    return static_cast<int>(o.value(QLatin1String(key)).toDouble());
}

qint64 int64(const QJsonObject &o, const char *key)
{
    return static_cast<qint64>(o.value(QLatin1String(key)).toDouble());
}

Facets parseFacets(const QJsonValue &v)
{
    Facets facets;
    if (!v.isObject())
        return facets;
    const QJsonObject fo = v.toObject();
    const QJsonValue fieldsVal = fo.value(QStringLiteral("fields"));
    if (!fieldsVal.isObject())
        return facets;
    facets.present = true;
    const QJsonObject fields = fieldsVal.toObject();
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        QList<Facet> list;
        if (it.value().isArray()) {
            for (const QJsonValue &fv : it.value().toArray()) {
                if (!fv.isObject())
                    continue;
                const QJsonObject fo2 = fv.toObject();
                Facet f;
                f.id = static_cast<qint64>(fo2.value(QStringLiteral("id")).toDouble());
                f.name = fo2.value(QStringLiteral("name")).toString();
                f.count = static_cast<int>(fo2.value(QStringLiteral("count")).toDouble());
                list.append(f);
            }
        }
        facets.fields.insert(it.key(), list);
    }
    return facets;
}

template <class T>
Paginated<T> parsePaginatedImpl(const QJsonValue &v, T (*parseOne)(const QJsonValue &))
{
    Paginated<T> out;
    if (!v.isObject())
        return out;
    const QJsonObject o = v.toObject();
    const QJsonValue next = o.value(QStringLiteral("next"));
    if (!next.isNull() && next.isString()) {
        out.next = next.toString();
        out.hasNext = true;
    }
    const QJsonValue prev = o.value(QStringLiteral("previous"));
    if (!prev.isNull() && prev.isString()) {
        out.previous = prev.toString();
        out.hasPrevious = true;
    }
    out.count = integer(o, "count");
    out.page = o.value(QStringLiteral("page")).toString();
    out.perPage = integer(o, "per_page");
    const QJsonArray results = o.value(QStringLiteral("results")).toArray();
    out.results.reserve(results.size());
    for (const QJsonValue &item : results)
        out.results.append(parseOne(item));
    out.facets = parseFacets(o.value(QStringLiteral("facets")));
    return out;
}

} // namespace

// --- Genre / Artist / Artists ----------------------------------------------

Genre parseGenre(const QJsonValue &v)
{
    Genre g;
    if (!v.isObject())
        return g;
    const QJsonObject o = v.toObject();
    g.id = int64(o, "id");
    g.name = str(o, "name");
    g.slug = str(o, "slug");
    return g;
}

Artist parseArtist(const QJsonValue &v)
{
    Artist a;
    if (!v.isObject())
        return a;
    const QJsonObject o = v.toObject();
    a.id = int64(o, "id");
    a.name = str(o, "name");
    a.slug = str(o, "slug");
    return a;
}

Artists parseArtists(const QJsonValue &v)
{
    Artists a;
    if (v.isArray())
        for (const QJsonValue &item : v.toArray())
            a.items.append(parseArtist(item));
    return a;
}

QString Artists::display(int limit, const QString &shortForm) const
{
    if (!shortForm.isEmpty() && limit >= 0 && items.size() > limit)
        return shortForm;
    QStringList names;
    for (const Artist &a : items)
        names.append(a.name);
    return names.join(QStringLiteral(", "));
}

QString artistDirectoryName(const Artist &a, const NamingPreferences &n)
{
    QMap<QString, QString> values{
        {QStringLiteral("id"), QString::number(a.id)},
        {QStringLiteral("name"), sanitizeForPath(a.name)},
        {QStringLiteral("slug"), a.slug},
    };
    return sanitizePath(parseTemplate(n.templ, values), n.whitespace);
}

// --- Key --------------------------------------------------------------------

Key parseKey(const QJsonValue &v)
{
    Key k;
    if (!v.isObject())
        return k;
    const QJsonObject o = v.toObject();
    k.name = str(o, "name");
    k.letter = str(o, "letter");
    const QJsonObject chord = o.value(QStringLiteral("chord_type")).toObject();
    k.chordType.name = chord.value(QStringLiteral("name")).toString();
    k.camelotNumber = integer(o, "camelot_number");
    k.camelotLetter = str(o, "camelot_letter");
    k.isFlat = o.value(QStringLiteral("is_flat")).toBool();
    k.isSharp = o.value(QStringLiteral("is_sharp")).toBool();
    return k;
}

QString Key::display(const QString &system) const
{
    if (system == QLatin1String("standard"))
        return name;
    if (system == QLatin1String("standard-short")) {
        QString symbol;
        if (isSharp)
            symbol = QStringLiteral("#");
        else if (isFlat)
            symbol = QStringLiteral("b");
        QString chord;
        if (chordType.name == QLatin1String("Minor"))
            chord = QStringLiteral("m");
        return letter + symbol + chord;
    }
    if (system == QLatin1String("openkey")) {
        int number = camelotNumber > 7 ? camelotNumber - 7 : camelotNumber + 5;
        QString letter;
        if (chordType.name == QLatin1String("Minor"))
            letter = QStringLiteral("m");
        else if (chordType.name == QLatin1String("Major"))
            letter = QStringLiteral("d");
        return QString::number(number) + letter;
    }
    if (system == QLatin1String("camelot"))
        return QString::number(camelotNumber) + camelotLetter;
    return {};
}

// --- Image ------------------------------------------------------------------

Image parseImage(const QJsonValue &v)
{
    Image i;
    if (!v.isObject())
        return i;
    const QJsonObject o = v.toObject();
    i.id = int64(o, "id");
    i.uri = str(o, "uri");
    i.dynamicUri = str(o, "dynamic_uri");
    return i;
}

QString Image::formattedUrl(const QString &size) const
{
    return QString(dynamicUri).replace(QStringLiteral("{w}x{h}"), size);
}

// --- Label ------------------------------------------------------------------

Label parseLabel(const QJsonValue &v)
{
    Label l;
    if (!v.isObject())
        return l;
    const QJsonObject o = v.toObject();
    l.id = int64(o, "id");
    l.name = str(o, "name");
    l.slug = str(o, "slug");
    l.created = dateOnlyFromRfc3339(str(o, "created"));
    l.updated = dateOnlyFromRfc3339(str(o, "updated"));
    return l;
}

QString Label::storeUrl() const
{
    return QStringLiteral("https://www.beatport.com/label/%1/%2").arg(slug).arg(id);
}

QString Label::directoryName(const NamingPreferences &n) const
{
    QMap<QString, QString> values{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("name"), sanitizeForPath(name)},
        {QStringLiteral("slug"), slug},
        {QStringLiteral("created_date"), created},
        {QStringLiteral("updated_date"), updated},
    };
    return sanitizePath(parseTemplate(n.templ, values), n.whitespace);
}

// --- Release ----------------------------------------------------------------

Release parseRelease(const QJsonValue &v)
{
    Release r;
    if (!v.isObject())
        return r;
    const QJsonObject o = v.toObject();
    r.id = int64(o, "id");
    r.name = sanitized(o.value(QStringLiteral("name")));
    r.slug = str(o, "slug");
    r.artists = parseArtists(o.value(QStringLiteral("artists")));
    r.remixers = parseArtists(o.value(QStringLiteral("remixers")));
    r.catalogNumber = sanitized(o.value(QStringLiteral("catalog_number")));
    r.upc = str(o, "upc");
    r.label = parseLabel(o.value(QStringLiteral("label")));
    r.date = str(o, "new_release_date");
    r.image = parseImage(o.value(QStringLiteral("image")));
    const QJsonObject bpm = o.value(QStringLiteral("bpm_range")).toObject();
    r.bpmRange.min = static_cast<int>(bpm.value(QStringLiteral("min")).toDouble());
    r.bpmRange.max = static_cast<int>(bpm.value(QStringLiteral("max")).toDouble());
    const QJsonArray tracks = o.value(QStringLiteral("tracks")).toArray();
    for (const QJsonValue &t : tracks)
        if (t.isString())
            r.trackUrls.append(t.toString());
    r.trackCount = integer(o, "track_count");
    r.url = str(o, "url");
    return r;
}

QString Release::storeUrl() const
{
    return QStringLiteral("https://www.beatport.com/release/%1/%2").arg(slug).arg(id);
}

QString Release::year() const
{
    return yearFromDate(date);
}

QString Release::directoryName(const NamingPreferences &n) const
{
    QMap<QString, QString> values{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("name"), sanitizeForPath(name)},
        {QStringLiteral("slug"), slug},
        {QStringLiteral("artists"), sanitizeForPath(artists.display(n.artistsLimit, n.artistsShortForm))},
        {QStringLiteral("remixers"), sanitizeForPath(remixers.display(n.artistsLimit, n.artistsShortForm))},
        {QStringLiteral("date"), date},
        {QStringLiteral("year"), year()},
        {QStringLiteral("track_count"), numberWithPadding(trackCount, trackCount, n.trackNumberPadding)},
        {QStringLiteral("bpm_range"), QStringLiteral("%1-%2").arg(bpmRange.min).arg(bpmRange.max)},
        {QStringLiteral("catalog_number"), sanitizeForPath(catalogNumber)},
        {QStringLiteral("upc"), upc},
        {QStringLiteral("label"), sanitizeForPath(label.name)},
    };
    return sanitizePath(parseTemplate(n.templ, values), n.whitespace);
}

// --- Track ------------------------------------------------------------------

Track parseTrack(const QJsonValue &v)
{
    Track t;
    if (!v.isObject())
        return t;
    const QJsonObject o = v.toObject();
    t.id = int64(o, "id");
    t.name = sanitized(o.value(QStringLiteral("name")));
    t.mixName = sanitized(o.value(QStringLiteral("mix_name")));
    t.slug = str(o, "slug");
    t.number = integer(o, "number");
    t.key = parseKey(o.value(QStringLiteral("key")));
    t.bpm = integer(o, "bpm");
    t.genre = parseGenre(o.value(QStringLiteral("genre")));
    const QJsonValue sub = o.value(QStringLiteral("sub_genre"));
    if (!sub.isNull() && sub.isObject())
        t.subgenre = parseGenre(sub);
    t.isrc = str(o, "isrc");
    t.length = str(o, "length");
    t.lengthMs = integer(o, "length_ms");
    t.artists = parseArtists(o.value(QStringLiteral("artists")));
    t.remixers = parseArtists(o.value(QStringLiteral("remixers")));
    t.publishDate = str(o, "publish_date");
    t.release = parseRelease(o.value(QStringLiteral("release")));
    t.url = str(o, "url");
    return t;
}

QString Track::storeUrl() const
{
    return QStringLiteral("https://www.beatport.com/track/%1/%2").arg(slug).arg(id);
}

QString Track::genreWithSubgenre(const QString &separator) const
{
    if (subgenre.has_value())
        return QStringLiteral("%1 %2 %3").arg(genre.name, separator, subgenre->name);
    return genre.name;
}

QString Track::subgenreOrGenre() const
{
    return subgenre.has_value() ? subgenre->name : genre.name;
}

QString Track::filename(const NamingPreferences &n) const
{
    const QString sub = subgenre.has_value() ? subgenre->name : QString();
    QMap<QString, QString> values{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("name"), sanitizeForPath(name)},
        {QStringLiteral("slug"), slug},
        {QStringLiteral("mix_name"), sanitizeForPath(mixName)},
        {QStringLiteral("artists"), sanitizeForPath(artists.display(n.artistsLimit, n.artistsShortForm))},
        {QStringLiteral("remixers"), sanitizeForPath(remixers.display(n.artistsLimit, n.artistsShortForm))},
        {QStringLiteral("number"), numberWithPadding(number, release.trackCount, n.trackNumberPadding)},
        {QStringLiteral("length"), durationDisplay(lengthMs)},
        {QStringLiteral("key"), key.display(n.keySystem)},
        {QStringLiteral("bpm"), QString::number(bpm)},
        {QStringLiteral("genre"), sanitizeForPath(genre.name)},
        {QStringLiteral("subgenre"), sanitizeForPath(sub)},
        {QStringLiteral("genre_with_subgenre"), sanitizeForPath(genreWithSubgenre(QStringLiteral("-")))},
        {QStringLiteral("subgenre_or_genre"), sanitizeForPath(subgenreOrGenre())},
        {QStringLiteral("isrc"), isrc},
        {QStringLiteral("label"), sanitizeForPath(release.label.name)},
    };
    return sanitizePath(parseTemplate(n.templ, values), n.whitespace);
}

TrackDownload parseTrackDownload(const QJsonValue &v)
{
    TrackDownload d;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        d.location = str(o, "location");
        d.streamQuality = str(o, "stream_quality");
    }
    return d;
}

TrackStream parseTrackStream(const QJsonValue &v)
{
    TrackStream s;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        s.url = str(o, "stream_url");
        s.sampleStartMs = integer(o, "sample_start_ms");
        s.sampleEndMs = integer(o, "sample_end_ms");
    }
    return s;
}

// --- Playlist / Chart --------------------------------------------------------

Playlist parsePlaylist(const QJsonValue &v)
{
    Playlist p;
    if (!v.isObject())
        return p;
    const QJsonObject o = v.toObject();
    p.id = int64(o, "id");
    p.name = str(o, "name");
    const QJsonArray genres = o.value(QStringLiteral("genres")).toArray();
    for (const QJsonValue &g : genres)
        if (g.isString())
            p.genres.append(g.toString());
    p.trackCount = integer(o, "track_count");
    const QJsonArray bpm = o.value(QStringLiteral("bpm_range")).toArray();
    if (bpm.size() >= 1 && bpm.at(0).isDouble())
        p.bpmRangeMin = static_cast<int>(bpm.at(0).toDouble());
    if (bpm.size() >= 2 && bpm.at(1).isDouble())
        p.bpmRangeMax = static_cast<int>(bpm.at(1).toDouble());
    p.lengthMs = integer(o, "length_ms");
    p.createdDate = dateOnlyFromRfc3339(str(o, "created_date"));
    p.updatedDate = dateOnlyFromRfc3339(str(o, "updated_date"));
    return p;
}

QString Playlist::directoryName(const NamingPreferences &n) const
{
    QString firstGenre;
    if (!genres.isEmpty())
        firstGenre = genres.first();
    QString bpmRange;
    if (bpmRangeMin.has_value() && bpmRangeMax.has_value())
        bpmRange = QStringLiteral("%1-%2").arg(*bpmRangeMin).arg(*bpmRangeMax);

    QMap<QString, QString> values{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("name"), sanitizeForPath(name)},
        {QStringLiteral("first_genre"), sanitizeForPath(firstGenre)},
        {QStringLiteral("track_count"), numberWithPadding(trackCount, trackCount, n.trackNumberPadding)},
        {QStringLiteral("bpm_range"), bpmRange},
        {QStringLiteral("length"), durationDisplay(lengthMs)},
        {QStringLiteral("created_date"), createdDate},
        {QStringLiteral("updated_date"), updatedDate},
    };
    return sanitizePath(parseTemplate(n.templ, values), n.whitespace);
}

PlaylistItem parsePlaylistItem(const QJsonValue &v)
{
    PlaylistItem i;
    if (!v.isObject())
        return i;
    const QJsonObject o = v.toObject();
    i.id = int64(o, "id");
    i.position = integer(o, "position");
    i.track = parseTrack(o.value(QStringLiteral("track")));
    return i;
}

Chart parseChart(const QJsonValue &v)
{
    Chart c;
    if (!v.isObject())
        return c;
    const QJsonObject o = v.toObject();
    c.id = int64(o, "id");
    c.name = str(o, "name");
    c.slug = str(o, "slug");
    c.trackCount = integer(o, "track_count");
    const QJsonObject person = o.value(QStringLiteral("person")).toObject();
    c.person.ownerName = person.value(QStringLiteral("owner_name")).toString();
    c.person.ownerSlug = person.value(QStringLiteral("owner_slug")).toString();
    const QJsonArray genres = o.value(QStringLiteral("genres")).toArray();
    for (const QJsonValue &g : genres)
        c.genres.append(parseGenre(g));
    c.addDate = dateOnlyFromRfc3339(str(o, "add_date"));
    c.changeDate = dateOnlyFromRfc3339(str(o, "change_date"));
    c.publishDate = dateOnlyFromRfc3339(str(o, "publish_date"));
    c.image = parseImage(o.value(QStringLiteral("image")));
    return c;
}

QString Chart::directoryName(const NamingPreferences &n) const
{
    QString firstGenre;
    if (!genres.isEmpty())
        firstGenre = genres.first().name;
    QMap<QString, QString> values{
        {QStringLiteral("id"), QString::number(id)},
        {QStringLiteral("name"), sanitizeForPath(name)},
        {QStringLiteral("slug"), slug},
        {QStringLiteral("first_genre"), sanitizeForPath(firstGenre)},
        {QStringLiteral("track_count"), numberWithPadding(trackCount, trackCount, n.trackNumberPadding)},
        {QStringLiteral("creator"), sanitizeForPath(person.ownerName)},
        {QStringLiteral("created_date"), addDate},
        {QStringLiteral("published_date"), publishDate},
        {QStringLiteral("updated_date"), changeDate},
    };
    return sanitizePath(parseTemplate(n.templ, values), n.whitespace);
}

// --- Search -----------------------------------------------------------------

SearchResults parseSearchResults(const QJsonValue &v)
{
    SearchResults s;
    if (!v.isObject())
        return s;
    const QJsonObject o = v.toObject();
    for (const QJsonValue &t : o.value(QStringLiteral("tracks")).toArray())
        s.tracks.append(parseTrack(t));
    for (const QJsonValue &r : o.value(QStringLiteral("releases")).toArray())
        s.releases.append(parseRelease(r));
    for (const QJsonValue &l : o.value(QStringLiteral("labels")).toArray())
        s.labels.append(parseLabel(l));
    return s;
}

// --- Paginated --------------------------------------------------------------

PaginatedTracks parsePaginatedTracks(const QJsonValue &v)
{
    return parsePaginatedImpl<Track>(v, parseTrack);
}

PaginatedReleases parsePaginatedReleases(const QJsonValue &v)
{
    return parsePaginatedImpl<Release>(v, parseRelease);
}

PaginatedGenres parsePaginatedGenres(const QJsonValue &v)
{
    return parsePaginatedImpl<Genre>(v, parseGenre);
}

PaginatedPlaylistItems parsePaginatedPlaylistItems(const QJsonValue &v)
{
    return parsePaginatedImpl<PlaylistItem>(v, parsePlaylistItem);
}

// --- Duration ---------------------------------------------------------------

QString durationDisplay(int lengthMs)
{
    const int totalSeconds = lengthMs / 1000;
    const int hours = totalSeconds / 3600;
    const int minutes = (totalSeconds % 3600) / 60;
    const int seconds = totalSeconds % 60;
    if (hours > 0)
        return QStringLiteral("%1-%2-%3")
            .arg(hours, 2, 10, QLatin1Char('0'))
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1-%2")
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}
