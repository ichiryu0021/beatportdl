// 纯逻辑冒烟测试（不跑网络）。main 返回非 0 即失败，每项通过打印 PASS。
//
// 与 Go 测试对应：
//   parseTemplate / sanitize* / sanitizedStringRaw
//       ← internal/beatport/utils_test.go
//   fnv64aHex / buildLatestParams
//       ← test/beatport/latest_pagination_test.go（FNV 为 auth.loginId 算法）
//   parseGenresHtml
//       ← test/beatport/genres_web_test.go
//   parseBeatportUrl
//       ← internal/beatport/links 相关测试
//   配置往返 / Key 制式 / HLS 播放列表
//       ← config / models / hls 各自的纯解析逻辑
//   Archiver
//       ← internal/sorting/genre_test.go（nil track 用例见报告存疑）

#include "core/util.h"
#include "core/models.h"
#include "core/config.h"
#include "core/logger.h"
#include "core/links.h"
#include "core/genres_scrape.h"
#include "core/hls.h"
#include "core/archiver.h"
#include "core/beatport.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QStringList>

#include <cstdio>

#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::fprintf(stderr, "FAIL: %s (%s:%d)\n", #cond, __FILE__, __LINE__);      \
            return 1;                                                                   \
        }                                                                               \
    } while (0)

#define RUN_TEST(fn)                                                                   \
    do {                                                                               \
        std::printf("---- %s\n", #fn);                                                 \
        if (fn() != 0)                                                                 \
            return 1;                                                                   \
        std::printf("PASS: %s\n", #fn);                                                \
    } while (0)

namespace {

// 1. parseTemplate：已知占位替换，未知占位原样保留。
int testParseTemplate()
{
    {
        const QMap<QString, QString> v{{QStringLiteral("name"),
                                        QStringLiteral("Strobe")}};
        CHECK(parseTemplate(QStringLiteral("{name} {unknown}"), v)
              == QStringLiteral("Strobe {unknown}"));
    }
    {
        const QMap<QString, QString> v{
            {QStringLiteral("artists"), QStringLiteral("Deadmau5")},
            {QStringLiteral("name"), QStringLiteral("Strobe")},
        };
        CHECK(parseTemplate(QStringLiteral("{artists} - {name}"), v)
              == QStringLiteral("Deadmau5 - Strobe"));
    }
    return 0;
}

// 2. sanitizeForPath：删除 '/' 与 '\\'，空白压缩。
int testSanitizeForPath()
{
    CHECK(sanitizeForPath(QStringLiteral("AC/DC \\ Back\tIn  Black"))
          == QStringLiteral("ACDC Back In Black"));
    return 0;
}

// 3. sanitizePath：删非法字符；可选空白替换。
int testSanitizePath()
{
    CHECK(sanitizePath(QStringLiteral("a<b>c:d\"e|f?g*h"), QString())
          == QStringLiteral("abcdefgh"));
    CHECK(sanitizePath(QStringLiteral("hello world"), QStringLiteral("_"))
          == QStringLiteral("hello_world"));
    return 0;
}

// 4. sanitizedStringRaw：Go JSON 测试里字面的 \n \t（反斜杠+字母）被删除，
//    然后空白压缩（对应 utils_test.go 的 bytes.Trim 后输入）。
int testSanitizedStringRaw()
{
    CHECK(sanitizedStringRaw(QStringLiteral("  Strobe\\n\\t (Original  Mix) "))
          == QStringLiteral("Strobe (Original Mix)"));
    return 0;
}

// 5. fnv64aHex：期望值按 Go FNV-1a 64 位算法独立计算
//    （offset basis 14695981039346656037, prime 1099511628211）。
int testFnv64a()
{
    CHECK(fnv64aHex(QStringLiteral("tester:secret"))
          == QStringLiteral("fe16df38562bd928"));
    return 0;
}

// 6. parseGenresHtml：两种锚点形态、&amp; 实体、slug 去重保留首个、按名排序。
int testParseGenresHtml()
{
    const QString html = QStringLiteral(
        "\n<html><body>\n"
        "<a href=\"/genre/deep-house/7766\">Deep House</a>\n"
        "<a href=\"/genre/techno/11894\" title=\"Techno\">Techno</a>\n"
        "<a href=\"/genre/techno/11894\" title=\"Techno (dup)\">Techno</a>\n"
        "<a href=\"/genre/drum-and-bass/8017\">Drum &amp; Bass</a>\n"
        "<span>ignore me</span>\n"
        "</body></html>\n");

    QString err;
    const QList<Genre> genres = parseGenresHtml(html, &err);
    CHECK(err.isEmpty());
    CHECK(genres.size() == 3);

    QMap<QString, QString> bySlug;
    for (const Genre &g : genres)
        bySlug.insert(g.slug, g.name);
    CHECK(bySlug.value(QStringLiteral("deep-house"))
          == QStringLiteral("Deep House"));
    CHECK(bySlug.value(QStringLiteral("techno")) == QStringLiteral("Techno"));
    // Go 用例只断言包含 "Drum"（&amp; 实体是否反转义未被 Go 测试覆盖）。
    CHECK(bySlug.value(QStringLiteral("drum-and-bass"))
              .contains(QStringLiteral("Drum")));

    for (int i = 1; i < genres.size(); ++i)
        CHECK(genres.at(i - 1).name <= genres.at(i).name); // 名称升序

    // 一个链接都没有 → 报错（server 回退 catalog API）。
    QString err2;
    const QList<Genre> none =
        parseGenresHtml(QStringLiteral("<html><body>no links</body></html>"), &err2);
    CHECK(none.isEmpty());
    CHECK(!err2.isEmpty());
    return 0;
}

// 7. Archiver：曲风目录、移入、重名 " (1)"、空白替换。
int testArchiver()
{
    const QString root =
        QDir::tempPath() + QStringLiteral("/bp_smoke_archiver_")
        + QString::number(QDateTime::currentMSecsSinceEpoch());
    CHECK(QDir().mkpath(root));

    auto stageFile = [](const QString &dir, const QString &name) -> QString {
        const QString path = QDir(dir).filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return {};
        f.write("x");
        f.close();
        return path;
    };

    int rc = 0;
    do {
        const QString staging = QDir(root).filePath(QStringLiteral("in"));
        CHECK(QDir().mkpath(staging));

        Archiver archiver(root, GenreMode::Genre, QChar('_'));

        Track track;
        track.genre.name = QStringLiteral("Deep House");

        QString err;
        const QString src1 = stageFile(staging, QStringLiteral("song.flac"));
        CHECK(!src1.isEmpty());
        const QString dst1 = archiver.file(track, src1, &err);
        if (!err.isEmpty()) { rc = 1; break; }
        CHECK(dst1 == QDir(root).filePath(QStringLiteral("Deep_House/song.flac")));
        CHECK(QFileInfo::exists(dst1));
        CHECK(!QFileInfo::exists(src1)); // 已移动

        const QString src2 = stageFile(staging, QStringLiteral("song.flac"));
        CHECK(!src2.isEmpty());
        const QString dst2 = archiver.file(track, src2, &err);
        CHECK(err.isEmpty());
        CHECK(dst2
              == QDir(root).filePath(QStringLiteral("Deep_House/song (1).flac")));
        CHECK(QFileInfo::exists(dst2));

        // 默认构造 Track（Go nil track 用例在引用签名下不可直接移植）：
        // genre 为空 → 文件落到 root 本身，不报错。
        Track empty;
        const QString src3 = stageFile(staging, QStringLiteral("loose.flac"));
        CHECK(!src3.isEmpty());
        const QString dst3 = archiver.file(empty, src3, &err);
        CHECK(err.isEmpty());
        CHECK(dst3 == QDir(root).filePath(QStringLiteral("loose.flac")));
    } while (false);

    QDir(root).removeRecursively();
    return rc;
}

// 8. parseBeatportUrl：六类链接、语言前缀、catalog 前缀、非法 host。
int testLinks()
{
    auto parse = [](const QString &u, Link *l) {
        QString err;
        return parseBeatportUrl(u, l, &err);
    };

    Link l;
    CHECK(parse(QStringLiteral("https://www.beatport.com/track/strobe/123"), &l));
    CHECK(l.type == LinkType::Track);
    CHECK(l.id == 123);

    CHECK(parse(QStringLiteral("https://www.beatport.com/release/for-lack/456"), &l));
    CHECK(l.type == LinkType::Release);
    CHECK(l.id == 456);

    CHECK(parse(QStringLiteral("https://www.beatport.com/chart/my-chart/321"), &l));
    CHECK(l.type == LinkType::Chart);
    CHECK(l.id == 321);

    // /playlist/<slug>/<id> 在 Go 里走 ChartLink。
    CHECK(parse(QStringLiteral("https://www.beatport.com/playlist/a-playlist/789"), &l));
    CHECK(l.type == LinkType::Chart);
    CHECK(l.id == 789);

    // /playlists 与 library/playlists 走 PlaylistLink。
    CHECK(parse(QStringLiteral("https://www.beatport.com/playlists/my/790"), &l));
    CHECK(l.type == LinkType::Playlist);
    CHECK(l.id == 790);
    CHECK(parse(QStringLiteral("https://www.beatport.com/library/playlists/791"), &l));
    CHECK(l.type == LinkType::Playlist);
    CHECK(l.id == 791);

    CHECK(parse(QStringLiteral("https://www.beatport.com/label/mau5trap/12"), &l));
    CHECK(l.type == LinkType::Label);
    CHECK(l.id == 12);

    CHECK(parse(QStringLiteral("https://www.beatport.com/artist/deadmau5/5"), &l));
    CHECK(l.type == LinkType::Artist);
    CHECK(l.id == 5);

    // 两字母语言前缀。
    CHECK(parse(QStringLiteral("https://www.beatport.com/de/track/strobe/123"), &l));
    CHECK(l.type == LinkType::Track);
    CHECK(l.id == 123);

    // api host + /de/catalog/tracks/123。
    CHECK(parse(QStringLiteral("https://api.beatport.com/tracks/123"), &l));
    CHECK(l.type == LinkType::Track);
    CHECK(l.id == 123);
    CHECK(parse(QStringLiteral("https://www.beatport.com/de/catalog/tracks/999"), &l));
    CHECK(l.type == LinkType::Track);
    CHECK(l.id == 999);

    // 非法 host。
    Link bad;
    CHECK(!parse(QStringLiteral("https://evil.com/track/x/1"), &bad));
    return 0;
}

// 9. buildLatestParams：分页/可流式/曲风转义/日期区间。
int testBuildLatestParams()
{
    const QString p = buildLatestParams(QStringLiteral("Tech House"), 2, 50,
                                        QStringLiteral("2024-01-01"),
                                        QStringLiteral("2024-12-31"));
    CHECK(p.contains(QStringLiteral("page=2")));
    CHECK(p.contains(QStringLiteral("per_page=50")));
    CHECK(p.contains(QStringLiteral("is_available_for_streaming=true")));
    CHECK(p.contains(QStringLiteral("genre_name=Tech+House")));
    CHECK(p.contains(QStringLiteral("new_release_date=2024-01-01:2024-12-31")));
    return 0;
}

// 10. 配置往返：save → parse，缺 tag_mappings 补默认；部分缺格式只补该格式。
int testConfigRoundtrip()
{
    const QString dir =
        QDir::tempPath() + QStringLiteral("/bp_smoke_cfg_")
        + QString::number(QDateTime::currentMSecsSinceEpoch());
    CHECK(QDir().mkpath(dir));
    int rc = 0;
    do {
        const QString path1 = QDir(dir).filePath(QStringLiteral("c1.yml"));
        {
            AppConfig cfg;
            cfg.applyDefaults();
            cfg.username = QStringLiteral("tester");
            cfg.password = QStringLiteral("secret");
            cfg.downloadsDirectory = dir;
            cfg.quality = QStringLiteral("high");
            QString err;
            CHECK(cfg.save(path1, &err));
        }
        {
            QString err;
            const auto loaded = parseConfigFile(path1, false, &err);
            CHECK(loaded != nullptr);
            CHECK(loaded->username == QStringLiteral("tester"));
            CHECK(loaded->downloadsDirectory == dir);
            CHECK(loaded->quality == QStringLiteral("high"));
            // 空 mappings → 全量默认。
            CHECK(loaded->tagMappings.contains(QStringLiteral("flac")));
            CHECK(loaded->tagMappings.contains(QStringLiteral("m4a")));
            CHECK(loaded->tagMappings[QStringLiteral("flac")]
                                         .contains(QStringLiteral("track_bpm")));
        }

        // 只写 flac 一个格式 → m4a 自动补默认，flac 保留用户内容（不被覆盖）。
        const QString path2 = QDir(dir).filePath(QStringLiteral("c2.yml"));
        {
            QFile f(path2);
            CHECK(f.open(QIODevice::WriteOnly | QIODevice::Text));
            const QByteArray yml =
                "username: tester\n"
                "password: secret\n"
                "downloads_directory: " + dir.toUtf8() + "\n"
                "quality: lossless\n"
                "tag_mappings:\n"
                "  flac:\n"
                "    track_name: CUSTOM_TITLE\n";
            f.write(yml);
            f.close();
        }
        {
            QString err;
            const auto loaded = parseConfigFile(path2, false, &err);
            CHECK(loaded != nullptr);
            CHECK(loaded->tagMappings.contains(QStringLiteral("m4a")));
            CHECK(loaded->tagMappings[QStringLiteral("m4a")].value(
                      QStringLiteral("track_name"))
                  == QStringLiteral("TITLE"));
            CHECK(loaded->tagMappings[QStringLiteral("flac")].value(
                      QStringLiteral("track_name"))
                  == QStringLiteral("CUSTOM_TITLE"));
            CHECK(!loaded->tagMappings[QStringLiteral("flac")]
                       .contains(QStringLiteral("track_bpm")));
        }
    } while (false);
    QDir(dir).removeRecursively();
    return rc;
}

// 11. Key::display 四种制式。
int testKeyDisplay()
{
    QJsonObject o;
    o[QStringLiteral("name")] = QStringLiteral("F# minor");
    o[QStringLiteral("letter")] = QStringLiteral("B");
    QJsonObject chord;
    chord[QStringLiteral("name")] = QStringLiteral("Minor");
    o[QStringLiteral("chord_type")] = chord;
    o[QStringLiteral("camelot_number")] = 8;
    o[QStringLiteral("camelot_letter")] = QStringLiteral("A");
    o[QStringLiteral("is_flat")] = false;
    o[QStringLiteral("is_sharp")] = true;

    const Key k = parseKey(o);
    CHECK(k.display(QStringLiteral("standard")) == QStringLiteral("F# minor"));
    CHECK(k.display(QStringLiteral("standard-short")) == QStringLiteral("B#m"));
    // openkey：n>7 → n-7 = 1，Minor → "1m"。
    CHECK(k.display(QStringLiteral("openkey")) == QStringLiteral("1m"));
    CHECK(k.display(QStringLiteral("camelot")) == QStringLiteral("8A"));
    return 0;
}

// 12. parseMediaPlaylist：段数/相对 URI 拼绝对/IV hex 解码。
int testParseMediaPlaylist()
{
    const QString m3u8 = QStringLiteral(
        "#EXTM3U\n"
        "#EXT-X-TARGETDURATION:6\n"
        "#EXT-X-KEY:METHOD=AES-128,URI=\"https://cdn.example.com/key.bin\","
        "IV=0x000102030405060708090A0B0C0D0E0F\n"
        "#EXTINF:5.0,\n"
        "seg1.aac\n"
        "#EXTINF:5.0,\n"
        "seg2.aac\n"
        "#EXTINF:5.0,\n"
        "seg3.aac\n"
        "#EXT-X-ENDLIST\n");

    HlsPlaylist pl;
    QString err;
    CHECK(parseMediaPlaylist(
              m3u8, QStringLiteral("https://cdn.example.com/hls/index.m3u8"),
              &pl, &err));
    CHECK(err.isEmpty());
    CHECK(pl.targetDuration == 6.0);
    CHECK(pl.segments.size() == 3);
    CHECK(pl.segments.first().uri
          == QStringLiteral("https://cdn.example.com/hls/seg1.aac"));
    CHECK(pl.hasKey);
    CHECK(pl.keyMethod == QStringLiteral("AES-128"));
    CHECK(pl.keyUri == QStringLiteral("https://cdn.example.com/key.bin"));
    CHECK(pl.keyIv.size() == 16);
    for (int i = 0; i < 16; ++i)
        CHECK(static_cast<unsigned char>(pl.keyIv.at(i)) == static_cast<unsigned char>(i));
    return 0;
}

} // namespace

int main()
{
    RUN_TEST(testParseTemplate);
    RUN_TEST(testSanitizeForPath);
    RUN_TEST(testSanitizePath);
    RUN_TEST(testSanitizedStringRaw);
    RUN_TEST(testFnv64a);
    RUN_TEST(testParseGenresHtml);
    RUN_TEST(testArchiver);
    RUN_TEST(testLinks);
    RUN_TEST(testBuildLatestParams);
    RUN_TEST(testConfigRoundtrip);
    RUN_TEST(testKeyDisplay);
    RUN_TEST(testParseMediaPlaylist);
    std::printf("\nall smoke tests passed\n");
    return 0;
}
