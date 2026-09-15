#include "genres_scrape.h"

#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <algorithm>

// 对应 Go genres_web.go 的两条正则（RE2 语法，\d 收敛为 ASCII [0-9]）：
//   href="/genre/deep-house/7766">Deep House</a>
//   href="/genre/deep-house/7766" title="Deep House">...</a>
namespace {

const QRegularExpression &genreLinkRe()
{
    static const QRegularExpression re(
        QStringLiteral("href=\"/genre/([a-z0-9-]+)/([0-9]+)\"[^>]*>([^<]{2,60})<"));
    return re;
}

const QRegularExpression &genreTitleRe()
{
    static const QRegularExpression re(
        QStringLiteral("href=\"/genre/([a-z0-9-]+)/[0-9]+\"[^>]*title=\"([^\"]{2,60})\""));
    return re;
}

} // namespace

QList<Genre> parseGenresHtml(const QString &html, QString *err)
{
    // 按 slug 去重，保留第一个命中的名字（Go map 插入语义）。
    QMap<QString, Genre> bySlug;

    auto it = genreLinkRe().globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const QString slug = m.captured(1);
        const QString name = m.captured(3).trimmed(); // strings.TrimSpace
        if (name.isEmpty())
            continue;
        if (!bySlug.contains(slug))
            bySlug.insert(slug, Genre{0, name, slug});
    }
    auto it2 = genreTitleRe().globalMatch(html);
    while (it2.hasNext()) {
        const QRegularExpressionMatch m = it2.next();
        const QString slug = m.captured(1);
        const QString name = m.captured(2).trimmed();
        if (name.isEmpty())
            continue;
        if (!bySlug.contains(slug))
            bySlug.insert(slug, Genre{0, name, slug});
    }

    if (bySlug.isEmpty()) {
        if (err)
            *err = QStringLiteral("no genre links found on beatport homepage");
        return {};
    }

    QList<Genre> out = bySlug.values();
    // sort.Slice(out, Name < Name)：Go 按 UTF-8 字节序；genre 名均为 BMP
    // 字符，QString 的 UTF-16 码元序与其一致。
    std::sort(out.begin(), out.end(),
              [](const Genre &a, const Genre &b) { return a.name < b.name; });

    if (err)
        err->clear();
    return out;
}
