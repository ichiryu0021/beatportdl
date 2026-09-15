#pragma once

#include <QString>
#include <QList>
#include <QJsonObject>
#include <QJsonValue>
#include <QSharedPointer>
#include <optional>

// ---------------------------------------------------------------------------
// Beatport API 实体模型。字段名、json tag、空值语义与 Go 版
// internal/beatport 严格一致。
// ---------------------------------------------------------------------------

struct Genre {
    qint64 id = 0;
    QString name;
    QString slug;
};
Genre parseGenre(const QJsonValue &v);

struct Artist {
    qint64 id = 0;
    QString name;
    QString slug;
};
Artist parseArtist(const QJsonValue &v);
QString artistDirectoryName(const Artist &a, const struct NamingPreferences &n);

struct Artists {
    QList<Artist> items;
    // 对应 (*Artists).Display：limit>=0 且人数超限时返回 shortForm，
    // 否则 ", " 连接全部名字。
    QString display(int limit, const QString &shortForm) const;
};
Artists parseArtists(const QJsonValue &v);

struct ChordType {
    QString name;
};

struct Key {
    QString name;           // json: name
    QString letter;         // json: letter
    ChordType chordType;    // json: chord_type.name
    int camelotNumber = 0;  // json: camelot_number
    QString camelotLetter;  // json: camelot_letter
    bool isFlat = false;    // json: is_flat
    bool isSharp = false;   // json: is_sharp

    // system: standard / standard-short / openkey / camelot
    QString display(const QString &system) const;
};
Key parseKey(const QJsonValue &v);

struct Image {
    qint64 id = 0;
    QString uri;
    QString dynamicUri;
    QString formattedUrl(const QString &size) const;
};
Image parseImage(const QJsonValue &v);

struct ReleaseBPMRange {
    int min = 0;
    int max = 0;
};

struct Label {
    qint64 id = 0;
    QString name;
    QString slug;
    QString created;  // yyyy-MM-dd（Go: time.Time）
    QString updated;

    QString storeUrl() const;
    QString directoryName(const struct NamingPreferences &n) const;
};
Label parseLabel(const QJsonValue &v);

struct Release {
    qint64 id = 0;
    QString name;        // SanitizedString
    QString slug;
    Artists artists;
    Artists remixers;
    QString catalogNumber; // SanitizedString
    QString upc;
    Label label;
    QString date;         // new_release_date
    Image image;
    ReleaseBPMRange bpmRange;
    QStringList trackUrls;
    int trackCount = 0;
    QString url;

    QString storeUrl() const;
    QString year() const;
    QString directoryName(const struct NamingPreferences &n) const;
};
Release parseRelease(const QJsonValue &v);

struct Track {
    qint64 id = 0;
    QString name;     // SanitizedString
    QString mixName;  // SanitizedString
    QString slug;
    int number = 0;
    Key key;
    int bpm = 0;
    Genre genre;
    std::optional<Genre> subgenre; // json: sub_genre，可为 null
    QString isrc;
    QString length;
    int lengthMs = 0;
    Artists artists;
    Artists remixers;
    QString publishDate;
    Release release;
    QString url;

    QString storeUrl() const;
    QString genreWithSubgenre(const QString &separator) const;
    QString subgenreOrGenre() const;
    QString filename(const struct NamingPreferences &n) const;
};
Track parseTrack(const QJsonValue &v);

struct TrackDownload {
    QString location;
    QString streamQuality;
};
TrackDownload parseTrackDownload(const QJsonValue &v);

struct TrackStream {
    QString url;
    int sampleStartMs = 0;
    int sampleEndMs = 0;
};
TrackStream parseTrackStream(const QJsonValue &v);

struct Playlist {
    qint64 id = 0;
    QString name;
    QStringList genres;
    int trackCount = 0;
    std::optional<int> bpmRangeMin; // []*int 两元素，可为 null
    std::optional<int> bpmRangeMax;
    int lengthMs = 0;
    QString createdDate;
    QString updatedDate;

    QString directoryName(const struct NamingPreferences &n) const;
};
Playlist parsePlaylist(const QJsonValue &v);

struct PlaylistItem {
    qint64 id = 0;
    int position = 0;
    Track track;
};
PlaylistItem parsePlaylistItem(const QJsonValue &v);

struct ChartPerson {
    QString ownerName;
    QString ownerSlug;
};

struct Chart {
    qint64 id = 0;
    QString name;
    QString slug;
    int trackCount = 0;
    ChartPerson person;
    QList<Genre> genres;
    QString addDate;
    QString changeDate;
    QString publishDate;
    Image image;

    QString directoryName(const struct NamingPreferences &n) const;
};
Chart parseChart(const QJsonValue &v);

struct SearchResults {
    QList<Track> tracks;
    QList<Release> releases;
    QList<Label> labels;
};
SearchResults parseSearchResults(const QJsonValue &v);

struct Facet {
    qint64 id = 0;
    QString name;
    int count = 0;
};

struct Facets {
    bool present = false;
    QMap<QString, QList<Facet>> fields;
};

template <class T>
struct Paginated {
    bool hasNext = false;
    QString next;
    bool hasPrevious = false;
    QString previous;
    int count = 0;
    QString page;
    int perPage = 0;
    QList<T> results;
    Facets facets;
};

// 具名分页类型（避免模板跨头文件展开问题）。
using PaginatedTracks = Paginated<Track>;
using PaginatedReleases = Paginated<Release>;
using PaginatedGenres = Paginated<Genre>;
using PaginatedPlaylistItems = Paginated<PlaylistItem>;

PaginatedTracks parsePaginatedTracks(const QJsonValue &v);
PaginatedReleases parsePaginatedReleases(const QJsonValue &v);
PaginatedGenres parsePaginatedGenres(const QJsonValue &v);
PaginatedPlaylistItems parsePaginatedPlaylistItems(const QJsonValue &v);

// NamingPreferences：cmd 与 internal/sorting 共用的命名参数。
struct NamingPreferences {
    QString templ;
    QString whitespace;
    int artistsLimit = 3;
    QString artistsShortForm;
    int trackNumberPadding = 2;
    QString keySystem;
};

// length_ms（毫秒）的 Duration.Display：HH-MM-SS 或 MM-SS。
QString durationDisplay(int lengthMs);
