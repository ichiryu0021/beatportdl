// 命令行交互，对齐 BPDL/cmd/interactions.go：
//   - createConfigInteractively：缺配置时问用户名/密码/下载目录/音质并写盘
//   - runCliBatch：位置参数 URL/.txt，enqueueURLs + runOnce
//   - runCliInteractive：主提示符（搜索/链接/label|artist 过滤向导）
// 控制台统一 std::getline + UTF-8。

#include "cli.h"
#include "downloader.h"
#include "config.h"
#include "beatport.h"
#include "links.h"
#include "util.h"

#include <QFile>
#include <QTextStream>
#include <QRegularExpression>
#include <QVector>
#include <QPair>
#include <QMap>

#include <cstdio>
#include <iostream>
#include <string>
#include <algorithm>

namespace {

bool g_quiet = false;

// Go prompt.Scanln / bufio.Reader：EOF 返回 false。
bool readLine(QString *out)
{
    std::string line;
    if (!std::getline(std::cin, line))
        return false;
    *out = QString::fromUtf8(line);
    return true;
}

// 逐行读 .txt（对应 Go readFileLines：ScanLines 去尾 \r\n，空行跳过）。
QStringList readUrlFile(const QString &path)
{
    QStringList urls;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        std::fprintf(stderr, "cannot open %s: %s\n",
                     path.toUtf8().constData(),
                     f.errorString().toUtf8().constData());
        return urls;
    }
    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (!line.isEmpty())
            urls.append(line);
    }
    return urls;
}

bool isTxtFile(const QString &arg)
{
    return arg.endsWith(QLatin1String(".txt"), Qt::CaseInsensitive);
}

QStringList collectArgs(const QStringList &args)
{
    QStringList all;
    for (const QString &a : args) {
        if (isTxtFile(a))
            all += readUrlFile(a);
        else
            all.append(a);
    }
    return all;
}

// ---------------------------------------------------------------------------
// search
// ---------------------------------------------------------------------------

void doSearch(Application *app, const QString &query)
{
    QString err;
    const auto res = app->bp->search(query, &err);
    if (!res) {
        std::fprintf(stderr, "search failed: %s\n", err.toUtf8().constData());
        return;
    }

    QVector<QString> urlByIndex; // 1-based：下标 0 占位
    urlByIndex.append(QString());

    if (!res->tracks.isEmpty()) {
        std::printf("\n[ Tracks ]\n");
        for (const Track &t : res->tracks) {
            urlByIndex.append(t.url);
            std::printf("%2d. %s - %s (%s) [%s]\n",
                        urlByIndex.size() - 1,
                        t.artists.display(3, QStringLiteral("VA")).toUtf8().constData(),
                        t.name.toUtf8().constData(),
                        t.mixName.toUtf8().constData(),
                        t.genre.name.toUtf8().constData());
        }
    }
    if (!res->releases.isEmpty()) {
        std::printf("\n[ Releases ]\n");
        for (const Release &r : res->releases) {
            urlByIndex.append(r.url);
            std::printf("%2d. %s - %s [%s]\n",
                        urlByIndex.size() - 1,
                        r.artists.display(3, QStringLiteral("VA")).toUtf8().constData(),
                        r.name.toUtf8().constData(),
                        r.label.name.toUtf8().constData());
        }
    }
    if (!res->labels.isEmpty()) {
        std::printf("\n[ Labels ]\n");
        for (const Label &l : res->labels) {
            urlByIndex.append(l.storeUrl());
            std::printf("%2d. %s\n",
                        urlByIndex.size() - 1,
                        l.name.toUtf8().constData());
        }
    }

    if (urlByIndex.size() <= 1) {
        std::printf("no results\n");
        return;
    }

    std::printf("\nEnter result numbers (space separated), empty to cancel: ");
    QString sel;
    if (!readLine(&sel))
        return;

    QStringList urls;
    const QStringList tokens =
        sel.split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
    for (const QString &tok : tokens) {
        bool ok = false;
        const int n = tok.toInt(&ok);
        if (!ok || n < 1 || n >= urlByIndex.size()) {
            std::printf("invalid result number: %s\n", tok.toUtf8().constData());
            continue;
        }
        urls.append(urlByIndex.at(n));
    }

    if (!urls.isEmpty()) {
        app->enqueueURLs(urls);
        app->runOnce();
    }
}

// ---------------------------------------------------------------------------
// filtersPrompt（label / artist 链接的 facet 过滤向导，对应 stats.go）
// ---------------------------------------------------------------------------

struct RankItem {
    QString name;
    int count = 0;
};

QVector<RankItem> rankMap(const QMap<QString, int> &counts)
{
    QVector<RankItem> items;
    items.reserve(counts.size());
    for (auto it = counts.begin(); it != counts.end(); ++it)
        items.append(RankItem{it.key(), it.value()});
    std::sort(items.begin(), items.end(),
              [](const RankItem &a, const RankItem &b) {
                  if (a.count != b.count)
                      return a.count > b.count;
                  return a.name < b.name;
              });
    return items;
}

// 逗号分隔多选；空/"*" = 跳过（back=null），"b" = 上一步（back=true）。
QStringList selectFromList(const QString &title, const QVector<RankItem> &items,
                           bool *back, bool *skip)
{
    *back = false;
    *skip = false;

    std::printf("\n%s\n", title.toUtf8().constData());
    for (int i = 0; i < items.size(); ++i) {
        std::printf("%2d. %-42s %d tracks\n",
                    i + 1,
                    items.at(i).name.toUtf8().constData(),
                    items.at(i).count);
    }
    std::printf("Select numbers (comma separated, empty/* to skip, b for back): ");

    QString line;
    if (!readLine(&line)) {
        *skip = true;
        return {};
    }
    line = line.trimmed();
    if (line.isEmpty() || line == QLatin1String("*")) {
        *skip = true;
        return {};
    }
    if (line == QLatin1String("b")) {
        *back = true;
        return {};
    }

    QStringList chosen;
    for (const QString &tok : line.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString t = tok.trimmed();
        bool ok = false;
        const int n = t.toInt(&ok);
        if (!ok || n < 1 || n > items.size())
            continue; // 非法编号忽略
        chosen.append(items.at(n - 1).name);
    }
    return chosen;
}

// normaliseDate / normaliseDateTo：对应 interactions.go 的日期补全。
QString normaliseDateFrom(QString s)
{
    s = s.trimmed();
    if (s.size() == 4)
        return s + QStringLiteral("-01-01");
    if (s.size() == 7)
        return s + QStringLiteral("-01");
    return s;
}

QString normaliseDateTo(QString s)
{
    s = s.trimmed();
    if (s.size() == 4)
        return s + QStringLiteral("-12-31");
    if (s.size() == 7)
        return s + QStringLiteral("-31");
    return s;
}

// 返回加好 query 参数的链接（取消返回空串）。
QString filtersPrompt(Application *app, const QString &rawURL)
{
    Link link;
    QString err;
    if (!app->bp->parseUrl(rawURL, &link, &err)) {
        std::fprintf(stderr, "invalid url: %s\n", err.toUtf8().constData());
        return {};
    }

    const QString facetParams = QStringLiteral("include_facets=true&per_page=1");
    Facets facets;
    qint64 total = 0;

    if (link.type == LinkType::Label) {
        const auto res = app->bp->getLabelReleases(link.id, 1, facetParams, &err);
        if (!res) {
            std::fprintf(stderr, "load label failed: %s\n", err.toUtf8().constData());
            return {};
        }
        facets = res->facets;
        total = res->count;
    } else if (link.type == LinkType::Artist) {
        const auto res = app->bp->getArtistTracks(link.id, 1, facetParams, &err);
        if (!res) {
            std::fprintf(stderr, "load artist failed: %s\n", err.toUtf8().constData());
            return {};
        }
        facets = res->facets;
        total = res->count;
    } else {
        return {};
    }

    // newEntityStats：genres / sub_genres(+id) / artists 三个 facet 字段。
    QMap<QString, int> genreCounts;
    QMap<QString, int> subgenreCounts;
    QMap<QString, qint64> subgenreIds;
    QMap<QString, int> artistCounts;

    const auto collect = [&](const char *fieldName, QMap<QString, int> &counts,
                             bool withIds = false) {
        const auto it =
            facets.fields.find(QString::fromLatin1(fieldName));
        if (it == facets.fields.end())
            return;
        for (const Facet &f : it.value()) {
            counts.insert(f.name, f.count);
            if (withIds)
                subgenreIds.insert(f.name, f.id);
        }
    };
    collect("genre", genreCounts);
    collect("sub_genre", subgenreCounts, true);
    collect("artists", artistCounts);

    const QVector<RankItem> genres = rankMap(genreCounts);
    const QVector<RankItem> subgenres = rankMap(subgenreCounts);
    const QVector<RankItem> artists = rankMap(artistCounts);

    QStringList selGenres;
    QStringList selSubgenres;
    QStringList selArtists;
    QString dateFrom;
    QString dateTo;

    enum Step { Genres = 0, Subgenres, Artists, DateFrom, DateTo, Confirm };
    int step = Genres;
    while (step <= Confirm) {
        bool back = false;
        bool skip = false;

        switch (step) {
        case Genres:
            if (genres.isEmpty()) {
                ++step;
                continue;
            }
            selGenres = selectFromList(QStringLiteral("Genres"), genres, &back, &skip);
            break;
        case Subgenres:
            if (subgenres.isEmpty()) {
                ++step;
                continue;
            }
            selSubgenres =
                selectFromList(QStringLiteral("Subgenres"), subgenres, &back, &skip);
            break;
        case Artists:
            if (artists.isEmpty()) {
                ++step;
                continue;
            }
            selArtists =
                selectFromList(QStringLiteral("Artists"), artists, &back, &skip);
            break;
        case DateFrom: {
            std::printf("Date from (YYYY, YYYY-MM or YYYY-MM-DD, empty for any, b back): ");
            QString raw;
            if (!readLine(&raw))
                return {};
            raw = raw.trimmed();
            if (raw == QLatin1String("b")) {
                back = true;
                break;
            }
            dateFrom = raw.isEmpty() ? QString() : normaliseDateFrom(raw);
            break;
        }
        case DateTo: {
            std::printf("Date to   (YYYY, YYYY-MM or YYYY-MM-DD, empty for any, b back): ");
            QString raw;
            if (!readLine(&raw))
                return {};
            raw = raw.trimmed();
            if (raw == QLatin1String("b")) {
                back = true;
                break;
            }
            dateTo = raw.isEmpty() ? QString() : normaliseDateTo(raw);
            break;
        }
        case Confirm: {
            std::printf("\nSummary (%lld total):\n", static_cast<long long>(total));
            if (!selGenres.isEmpty())
                std::printf("  genres:   %s\n", selGenres.join(", ").toUtf8().constData());
            if (!selSubgenres.isEmpty())
                std::printf("  sub:      %s\n", selSubgenres.join(", ").toUtf8().constData());
            if (!selArtists.isEmpty())
                std::printf("  artists:  %s\n", selArtists.join(", ").toUtf8().constData());
            if (!dateFrom.isEmpty() || !dateTo.isEmpty())
                std::printf("  dates:    %s : %s\n",
                            dateFrom.toUtf8().constData(),
                            dateTo.toUtf8().constData());
            std::printf("Proceed? [y/N/b]: ");
            QString raw;
            if (!readLine(&raw))
                return {};
            raw = raw.trimmed().toLower();
            if (raw == QLatin1String("b")) {
                back = true;
                break;
            }
            if (raw == QLatin1String("y") || raw == QLatin1String("yes"))
                ++step;
            else
                return {}; // 非确认：取消
            break;
        }
        }

        if (back) {
            // 回退到上一个非空步骤。
            int prev = step - 1;
            while (prev >= 0) {
                if (prev == Genres && !genres.isEmpty())
                    break;
                if (prev == Subgenres && !subgenres.isEmpty())
                    break;
                if (prev == Artists && !artists.isEmpty())
                    break;
                if (prev == DateFrom || prev == DateTo)
                    break;
                --prev;
            }
            step = (prev >= 0) ? prev : Genres;
            continue;
        }
        // Confirm 确认时已把 step 置为 Confirm+1。
        if (step <= Confirm)
            ++step;
    }

    // 拼参数：genre_name / artist_name 逐项 queryEscape；sub_genre_id 用十进制 id 不转义。
    QStringList parts;
    if (!selGenres.isEmpty()) {
        QStringList enc;
        for (const QString &g : selGenres)
            enc.append(QString::fromUtf8(queryEscape(g)));
        parts.append(QStringLiteral("genre_name=") + enc.join(QLatin1Char(',')));
    }
    if (!selSubgenres.isEmpty()) {
        QStringList ids;
        for (const QString &name : selSubgenres)
            ids.append(QString::number(subgenreIds.value(name)));
        parts.append(QStringLiteral("sub_genre_id=") + ids.join(QLatin1Char(',')));
    }
    if (!selArtists.isEmpty()) {
        QStringList enc;
        for (const QString &a : selArtists)
            enc.append(QString::fromUtf8(queryEscape(a)));
        parts.append(QStringLiteral("artist_name=") + enc.join(QLatin1Char(',')));
    }
    if (!dateFrom.isEmpty() || !dateTo.isEmpty())
        parts.append(QStringLiteral("new_release_date=%1:%2").arg(dateFrom, dateTo));

    if (parts.isEmpty())
        return rawURL;

    const QString paramsTotal = parts.join(QLatin1Char('&'));
    QString out = rawURL;
    out += link.params.isEmpty() ? QLatin1Char('?') + paramsTotal
                                  : QLatin1Char('&') + paramsTotal;
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// 导出接口
// ---------------------------------------------------------------------------

void consoleProgress(const QString &prefix, int percent)
{
    if (g_quiet)
        return;
    std::printf("\r%s %3d%%", prefix.toUtf8().constData(), percent);
    std::fflush(stdout);
}

void consoleProgressDone(const QString &prefix)
{
    if (g_quiet)
        return;
    std::printf("\r%s 100%%\n", prefix.toUtf8().constData());
    std::fflush(stdout);
}

bool createConfigInteractively(const QString &configPath, QString *err)
{
    auto ask = [](const char *prompt) {
        std::printf("%s", prompt);
        std::string s;
        std::getline(std::cin, s);
        return QString::fromUtf8(s).trimmed();
    };

    const QString username = ask("Username: ");
    const QString password = ask("Password: ");
    const QString downloads = ask("Downloads directory: ");

    QString quality;
    while (true) {
        std::printf("Select audio quality:\n");
        std::printf("  1. lossless\n");
        std::printf("  2. high\n");
        std::printf("  3. medium\n");
        std::printf("  4. medium-hls\n");
        const QString choice = ask("Enter choice [1-4] (default 1): ");
        if (choice.isEmpty() || choice == QLatin1String("1")) {
            quality = QStringLiteral("lossless");
            break;
        }
        if (choice == QLatin1String("2")) {
            quality = QStringLiteral("high");
            break;
        }
        if (choice == QLatin1String("3")) {
            quality = QStringLiteral("medium");
            break;
        }
        if (choice == QLatin1String("4")) {
            quality = QStringLiteral("medium-hls");
            break;
        }
        std::printf("invalid choice, please try again\n");
    }

    AppConfig cfg;
    cfg.applyDefaults();
    cfg.username = username;
    cfg.password = password;
    cfg.downloadsDirectory = downloads;
    cfg.quality = quality;

    QString saveErr;
    if (!cfg.save(configPath, &saveErr)) {
        if (err)
            *err = QStringLiteral("cannot write config %1: %2").arg(configPath, saveErr);
        return false;
    }
    return true;
}

int runCliBatch(Application *app, const QStringList &args, bool quiet)
{
    g_quiet = quiet;
    app->useConsoleProgress = !quiet;

    const QStringList urls = collectArgs(args);
    if (urls.isEmpty()) {
        std::fprintf(stderr, "no urls given\n");
        return 1;
    }
    app->enqueueURLs(urls);
    app->runOnce();
    return 0;
}

int runCliInteractive(Application *app, bool quiet)
{
    g_quiet = quiet;
    app->useConsoleProgress = !quiet;

    while (true) {
        std::printf("\nEnter url or search query: ");
        QString input;
        if (!readLine(&input))
            break; // EOF
        input = input.trimmed();
        if (input.isEmpty())
            continue;

        if (isTxtFile(input)) {
            const QStringList urls = readUrlFile(input);
            if (!urls.isEmpty()) {
                app->enqueueURLs(urls);
                app->runOnce();
            }
            continue;
        }

        const QString wwwPrefix = QStringLiteral("https://www.beatport.com");
        if (input.startsWith(wwwPrefix)) {
            if (input.contains(QLatin1String("/label/"))
                || input.contains(QLatin1String("/artist/"))) {
                const QString finalUrl = filtersPrompt(app, input);
                if (!finalUrl.isEmpty()) {
                    app->enqueueURLs({finalUrl});
                    app->runOnce();
                }
            } else {
                app->enqueueURLs({input});
                app->runOnce();
            }
            continue;
        }

        doSearch(app, input);
    }
    return 0;
}
