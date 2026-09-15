#include "config.h"
#include "util.h"

#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QTextStream>
#include <QStandardPaths>
#include <QRegularExpression>

const QString kDefaultCoverSize = QStringLiteral("1400x1400");
const QStringList kSupportedTrackExistsOptions{
    QStringLiteral("error"), QStringLiteral("skip"),
    QStringLiteral("overwrite"), QStringLiteral("update")};
const QStringList kSupportedKeySystems{
    QStringLiteral("standard"), QStringLiteral("standard-short"),
    QStringLiteral("openkey"), QStringLiteral("camelot")};

namespace {
QString g_configOverride;

// --- 极简 YAML 子集：扁平 key: value + tag_mappings 两级嵌套 map -----------

QString unquoteYaml(const QString &raw)
{
    QString v = raw.trimmed();
    if (v.size() >= 2 && v.startsWith('"') && v.endsWith('"')) {
        v = v.mid(1, v.size() - 2);
        v.replace(QStringLiteral("\\n"), QStringLiteral("\n"))
         .replace(QStringLiteral("\\t"), QStringLiteral("\t"))
         .replace(QStringLiteral("\\\""), QStringLiteral("\""))
         .replace(QStringLiteral("\\\\"), QStringLiteral("\\"));
        return v;
    }
    if (v.size() >= 2 && v.startsWith('\'') && v.endsWith('\'')) {
        v = v.mid(1, v.size() - 2);
        v.replace(QStringLiteral("''"), QStringLiteral("'"));
        return v;
    }
    return v;
}

// 去掉行内注释（未加引号情况下的 " #"）。
QString stripComment(const QString &line)
{
    bool inSingle = false, inDouble = false;
    for (int i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (c == '\'' && !inDouble)
            inSingle = !inSingle;
        else if (c == '"' && !inSingle)
            inDouble = !inDouble;
        else if (c == '#' && !inSingle && !inDouble && i > 0 && line.at(i - 1) == ' ')
            return line.left(i);
    }
    return line;
}

bool parseBool(const QString &s, bool fallback)
{
    const QString v = s.trimmed();
    if (v == QLatin1String("true"))
        return true;
    if (v == QLatin1String("false"))
        return false;
    return fallback;
}

bool parseInt(const QString &s, int fallback)
{
    bool ok = false;
    const int v = s.trimmed().toInt(&ok);
    return ok ? v : fallback;
}

bool yamlNeedsQuote(const QString &s)
{
    if (s.isEmpty())
        return true;
    static const QStringList literalLike{
        QStringLiteral("true"), QStringLiteral("false"),
        QStringLiteral("null"), QStringLiteral("yes"), QStringLiteral("no"),
        QStringLiteral("~")};
    if (literalLike.contains(s.trimmed().toLower()))
        return true;
    const QChar first = s.at(0);
    if (QStringLiteral("-?:,[]{}#&*!|>'\"%@`").contains(first))
        return true;
    if (s != s.trimmed())
        return true;
    if (s.contains(QStringLiteral(": ")) || s.contains(QStringLiteral(" #"))
        || s.contains(QLatin1Char('\n')) || s.contains(QLatin1Char('\t')))
        return true;
    bool ok = false;
    s.toInt(&ok);
    return ok;
}

QString yamlQuote(const QString &s)
{
    QString out = s;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"))
       .replace(QLatin1Char('"'), QStringLiteral("\\\""))
       .replace(QLatin1Char('\n'), QStringLiteral("\\n"))
       .replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    return QStringLiteral("\"%1\"").arg(out);
}

QString yamlScalar(const QString &s)
{
    return yamlNeedsQuote(s) ? yamlQuote(s) : s;
}

} // namespace

void AppConfig::applyDefaults()
{
    quality = QStringLiteral("lossless");
    coverSize = kDefaultCoverSize;
    trackFileTemplate = QStringLiteral("{number}. {artists} - {name} ({mix_name})");
    releaseDirectoryTemplate = QStringLiteral("[{catalog_number}] {artists} - {name}");
    playlistDirectoryTemplate = QStringLiteral("{name} [{created_date}]");
    chartDirectoryTemplate = QStringLiteral("{name} [{published_date}]");
    labelDirectoryTemplate = QStringLiteral("{name} [{updated_date}]");
    artistDirectoryTemplate = QStringLiteral("{name}");
    artistsLimit = 3;
    artistsShortForm = QStringLiteral("VA");
    keySystem = QStringLiteral("standard-short");
    trackExists = QStringLiteral("update");
    trackNumberPadding = 2;
    fixTags = true;
    showProgress = true;
    maxGlobalWorkers = 15;
    maxDownloadWorkers = 15;
    genreSortMode = QStringLiteral("genre");
    verifyIntegrity = true;
    proxyMode = QStringLiteral("auto");
}

QStringList supportedTagMappingFields()
{
    return {
        QStringLiteral("track_id"), QStringLiteral("track_url"),
        QStringLiteral("track_name"), QStringLiteral("track_artists"),
        QStringLiteral("track_remixers"), QStringLiteral("track_artists_limited"),
        QStringLiteral("track_remixers_limited"), QStringLiteral("track_number"),
        QStringLiteral("track_number_with_padding"),
        QStringLiteral("track_number_with_total"), QStringLiteral("track_genre"),
        QStringLiteral("track_subgenre"), QStringLiteral("track_genre_with_subgenre"),
        QStringLiteral("track_subgenre_or_genre"), QStringLiteral("track_key"),
        QStringLiteral("track_bpm"), QStringLiteral("track_isrc"),
        QStringLiteral("release_id"), QStringLiteral("release_url"),
        QStringLiteral("release_name"), QStringLiteral("release_artists"),
        QStringLiteral("release_remixers"), QStringLiteral("release_artists_limited"),
        QStringLiteral("release_remixers_limited"), QStringLiteral("release_date"),
        QStringLiteral("release_year"), QStringLiteral("release_track_count"),
        QStringLiteral("release_track_count_with_padding"),
        QStringLiteral("release_catalog_number"), QStringLiteral("release_upc"),
        QStringLiteral("release_label"), QStringLiteral("release_label_url"),
    };
}

QMap<QString, QMap<QString, QString>> defaultTagMappings()
{
    QMap<QString, QMap<QString, QString>> m;
    m[QStringLiteral("flac")] = {
        {QStringLiteral("track_name"), QStringLiteral("TITLE")},
        {QStringLiteral("track_artists"), QStringLiteral("ARTIST")},
        {QStringLiteral("track_number"), QStringLiteral("TRACKNUMBER")},
        {QStringLiteral("track_subgenre_or_genre"), QStringLiteral("GENRE")},
        {QStringLiteral("track_key"), QStringLiteral("KEY")},
        {QStringLiteral("track_bpm"), QStringLiteral("BPM")},
        {QStringLiteral("track_isrc"), QStringLiteral("ISRC")},
        {QStringLiteral("release_name"), QStringLiteral("ALBUM")},
        {QStringLiteral("release_artists"), QStringLiteral("ALBUMARTIST")},
        {QStringLiteral("release_date"), QStringLiteral("DATE")},
        {QStringLiteral("release_track_count"), QStringLiteral("TOTALTRACKS")},
        {QStringLiteral("release_catalog_number"), QStringLiteral("CATALOGNUMBER")},
        {QStringLiteral("release_label"), QStringLiteral("LABEL")},
    };
    m[QStringLiteral("m4a")] = {
        {QStringLiteral("track_name"), QStringLiteral("TITLE")},
        {QStringLiteral("track_artists"), QStringLiteral("ARTIST")},
        {QStringLiteral("track_number"), QStringLiteral("TRACKNUMBER")},
        {QStringLiteral("track_genre"), QStringLiteral("GENRE")},
        {QStringLiteral("track_key"), QStringLiteral("KEY")},
        {QStringLiteral("track_bpm"), QStringLiteral("BPM")},
        {QStringLiteral("track_isrc"), QStringLiteral("ISRC")},
        {QStringLiteral("release_name"), QStringLiteral("ALBUM")},
        {QStringLiteral("release_artists"), QStringLiteral("ALBUMARTIST")},
        {QStringLiteral("release_date"), QStringLiteral("DATE")},
        {QStringLiteral("release_track_count"), QStringLiteral("TOTALTRACKS")},
        {QStringLiteral("release_catalog_number"), QStringLiteral("CATALOGNUMBER")},
        {QStringLiteral("release_label"), QStringLiteral("LABEL")},
    };
    return m;
}

QString validateTagMappings(const QMap<QString, QMap<QString, QString>> &m)
{
    static const QStringList formats{QStringLiteral("flac"), QStringLiteral("m4a")};
    const QStringList fields = supportedTagMappingFields();
    for (auto fmtIt = m.begin(); fmtIt != m.end(); ++fmtIt) {
        if (!formats.contains(fmtIt.key()))
            return QStringLiteral("invalid tag mapping format '%1'").arg(fmtIt.key());
        for (auto fieldIt = fmtIt.value().begin(); fieldIt != fmtIt.value().end(); ++fieldIt) {
            if (!fields.contains(fieldIt.key()))
                return QStringLiteral("invalid tag mapping field '%1'").arg(fieldIt.key());
        }
    }
    return {};
}

bool ffmpegInstalled()
{
    // exe 旁 / 工作目录优先（便携化），再查 PATH。
#ifdef Q_OS_WIN
    const QString ffmpegName = QStringLiteral("ffmpeg.exe");
#else
    const QString ffmpegName = QStringLiteral("ffmpeg");
#endif
    const QString exeLocal = QDir(QCoreApplication::applicationDirPath()).filePath(ffmpegName);
    if (QFileInfo::exists(exeLocal))
        return true;
    return !QStandardPaths::findExecutable(ffmpegName).isEmpty();
}

QSharedPointer<AppConfig> parseConfigFile(const QString &filePath, bool lenient, QString *err)
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (err) *err = f.errorString();
        return nullptr;
    }

    auto cfg = QSharedPointer<AppConfig>::create();
    cfg->applyDefaults();

    // 逐行解析。tag_mappings 支持两级嵌套，用缩进层级处理。
    QString inTagFormat;
    bool inTagMappings = false;
    bool proxyModeSeen = false;

    QTextStream in(&f);
    in.setEncoding(QStringConverter::Utf8);
    while (!in.atEnd()) {
        const QString rawLine = in.readLine();
        const QString line = stripComment(rawLine);
        if (line.trimmed().isEmpty())
            continue;

        const int indent = line.size() - line.trimmed().size();
        const QString content = line.trimmed();
        const int colon = content.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;
        const QString key = content.left(colon).trimmed();
        const QString value = unquoteYaml(content.mid(colon + 1));

        if (indent == 0) {
            inTagFormat.clear();
            inTagMappings = (key == QLatin1String("tag_mappings"));
            if (inTagMappings)
                continue;

            const auto setStr = [&](QString &dst) { dst = value; };
            const auto setBool = [&](bool &dst) { dst = parseBool(value, dst); };
            const auto setInt = [&](int &dst) { dst = parseInt(value, dst); };

            if (key == QLatin1String("username")) setStr(cfg->username);
            else if (key == QLatin1String("password")) setStr(cfg->password);
            else if (key == QLatin1String("quality")) setStr(cfg->quality);
            else if (key == QLatin1String("write_error_log")) setBool(cfg->writeErrorLog);
            else if (key == QLatin1String("show_progress")) setBool(cfg->showProgress);
            else if (key == QLatin1String("max_global_workers")) setInt(cfg->maxGlobalWorkers);
            else if (key == QLatin1String("max_download_workers")) setInt(cfg->maxDownloadWorkers);
            else if (key == QLatin1String("downloads_directory")) setStr(cfg->downloadsDirectory);
            else if (key == QLatin1String("sort_by_context")) setBool(cfg->sortByContext);
            else if (key == QLatin1String("sort_by_label")) setBool(cfg->sortByLabel);
            else if (key == QLatin1String("force_release_directories")) setBool(cfg->forceReleaseDirectories);
            else if (key == QLatin1String("sort_by_genre")) setBool(cfg->sortByGenre);
            else if (key == QLatin1String("genre_sort_mode")) setStr(cfg->genreSortMode);
            else if (key == QLatin1String("verify_integrity")) setBool(cfg->verifyIntegrity);
            else if (key == QLatin1String("track_exists")) setStr(cfg->trackExists);
            else if (key == QLatin1String("track_number_padding")) setInt(cfg->trackNumberPadding);
            else if (key == QLatin1String("release_directory_template")) setStr(cfg->releaseDirectoryTemplate);
            else if (key == QLatin1String("playlist_directory_template")) setStr(cfg->playlistDirectoryTemplate);
            else if (key == QLatin1String("chart_directory_template")) setStr(cfg->chartDirectoryTemplate);
            else if (key == QLatin1String("label_directory_template")) setStr(cfg->labelDirectoryTemplate);
            else if (key == QLatin1String("artist_directory_template")) setStr(cfg->artistDirectoryTemplate);
            else if (key == QLatin1String("track_file_template")) setStr(cfg->trackFileTemplate);
            else if (key == QLatin1String("whitespace_character")) setStr(cfg->whitespaceCharacter);
            else if (key == QLatin1String("artists_limit")) setInt(cfg->artistsLimit);
            else if (key == QLatin1String("artists_short_form")) setStr(cfg->artistsShortForm);
            else if (key == QLatin1String("key_system")) setStr(cfg->keySystem);
            else if (key == QLatin1String("cover_size")) setStr(cfg->coverSize);
            else if (key == QLatin1String("keep_cover")) setBool(cfg->keepCover);
            else if (key == QLatin1String("fix_tags")) setBool(cfg->fixTags);
            else if (key == QLatin1String("proxy")) setStr(cfg->proxy);
            else if (key == QLatin1String("proxy_mode")) {
                setStr(cfg->proxyMode);
                proxyModeSeen = true;
            }
            else if (key == QLatin1String("theme")) setStr(cfg->theme);
            else if (key == QLatin1String("language")) setStr(cfg->language);
            continue;
        }

        if (!inTagMappings)
            continue;
        if (indent == 2 && value.isEmpty()) {
            // 格式层：flac: / m4a:
            inTagFormat = key;
            cfg->tagMappings.insert(inTagFormat, {});
        } else if (indent >= 4 && !inTagFormat.isEmpty()) {
            cfg->tagMappings[inTagFormat].insert(key, value);
        }
    }
    f.close();

    // 代理模式兼容与校验：
    // - 旧配置无 proxy_mode：proxy 非空 → manual，空 → auto（开箱即用跟随系统）。
    // - proxy_mode 非法值：报错；manual 缺 URL：报错。
    if (!proxyModeSeen) {
        cfg->proxyMode = cfg->proxy.trimmed().isEmpty()
                             ? QStringLiteral("auto")
                             : QStringLiteral("manual");
    }
    if (cfg->proxyMode != QLatin1String("off")
        && cfg->proxyMode != QLatin1String("auto")
        && cfg->proxyMode != QLatin1String("manual")) {
        if (err)
            *err = QStringLiteral("invalid proxy_mode \"%1\" (want off/auto/manual)")
                       .arg(cfg->proxyMode);
        return nullptr;
    }
    if (cfg->proxyMode == QLatin1String("manual") && cfg->proxy.trimmed().isEmpty()) {
        if (err)
            *err = QStringLiteral("proxy_mode manual requires a proxy url");
        return nullptr;
    }
    // theme 非法值不致命：回退默认浅色，避免旧/手写配置启动失败。
    if (cfg->theme != QLatin1String("light") && cfg->theme != QLatin1String("dark"))
        cfg->theme = QStringLiteral("light");
    // language: system（跟随系统）/ en / zh，非法值回退 system。
    if (cfg->language != QLatin1String("system") &&
        cfg->language != QLatin1String("en") &&
        cfg->language != QLatin1String("zh"))
        cfg->language = QStringLiteral("system");

    // worker 数必须为正：0 或负值会让两级 QSemaphore 初始许可为 0，
    // 所有下载线程永久阻塞（表现为完全不下载）。clamp 到 [1, 64]。
    cfg->maxGlobalWorkers = qBound(1, cfg->maxGlobalWorkers, 64);
    cfg->maxDownloadWorkers = qBound(1, cfg->maxDownloadWorkers, 64);

    if (!lenient && (cfg->username.isEmpty() || cfg->password.isEmpty())) {
        if (err) *err = QStringLiteral("username or password is not provided");
        return nullptr;
    }

    // parseValidate
    if (cfg->quality == QLatin1String("medium-hls") && !ffmpegInstalled()) {
        if (err) *err = QStringLiteral("ffmpeg not found");
        return nullptr;
    }
    if (!cfg->tagMappings.isEmpty()) {
        const QString mappingErr = validateTagMappings(cfg->tagMappings);
        if (!mappingErr.isEmpty()) {
            if (err) *err = mappingErr;
            return nullptr;
        }
        const auto defaults = defaultTagMappings();
        if (!cfg->tagMappings.contains(QStringLiteral("flac")))
            cfg->tagMappings[QStringLiteral("flac")] = defaults[QStringLiteral("flac")];
        if (!cfg->tagMappings.contains(QStringLiteral("m4a")))
            cfg->tagMappings[QStringLiteral("m4a")] = defaults[QStringLiteral("m4a")];
    } else {
        cfg->tagMappings = defaultTagMappings();
    }
    if (!kSupportedKeySystems.contains(cfg->keySystem)) {
        if (err) *err = QStringLiteral("invalid key system");
        return nullptr;
    }
    if (cfg->downloadsDirectory.isEmpty()) {
        if (err) *err = QStringLiteral("no downloads directory provided");
        return nullptr;
    }
    if (!kSupportedTrackExistsOptions.contains(cfg->trackExists)) {
        if (err) *err = QStringLiteral("invalid track exists behavior");
        return nullptr;
    }
    if (cfg->trackNumberPadding > 10 || cfg->trackNumberPadding < 0) {
        if (err) *err = QStringLiteral("invalid track number padding");
        return nullptr;
    }
    return cfg;
}

bool AppConfig::save(const QString &filePath, QString *err) const
{
    QDir().mkpath(QFileInfo(filePath).absolutePath());
    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (err) *err = QStringLiteral("create file: %1").arg(f.errorString());
        return false;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);

    auto writeStr = [&](const char *key, const QString &v) {
        if (!v.isEmpty())
            out << QLatin1String(key) << ": " << yamlScalar(v) << '\n';
    };
    auto writeBool = [&](const char *key, bool v) {
        if (v)
            out << QLatin1String(key) << ": true\n";
    };
    auto writeInt = [&](const char *key, int v) {
        if (v != 0)
            out << QLatin1String(key) << ": " << v << '\n';
    };

    writeStr("username", username);
    writeStr("password", password);
    writeStr("quality", quality);
    writeBool("write_error_log", writeErrorLog);
    writeBool("show_progress", showProgress);
    writeInt("max_global_workers", maxGlobalWorkers);
    writeInt("max_download_workers", maxDownloadWorkers);
    writeStr("downloads_directory", downloadsDirectory);
    writeBool("sort_by_context", sortByContext);
    writeBool("sort_by_label", sortByLabel);
    writeBool("force_release_directories", forceReleaseDirectories);
    writeBool("sort_by_genre", sortByGenre);
    writeStr("genre_sort_mode", genreSortMode);
    writeBool("verify_integrity", verifyIntegrity);
    writeStr("track_exists", trackExists);
    writeInt("track_number_padding", trackNumberPadding);
    writeStr("release_directory_template", releaseDirectoryTemplate);
    writeStr("playlist_directory_template", playlistDirectoryTemplate);
    writeStr("chart_directory_template", chartDirectoryTemplate);
    writeStr("label_directory_template", labelDirectoryTemplate);
    writeStr("artist_directory_template", artistDirectoryTemplate);
    writeStr("track_file_template", trackFileTemplate);
    writeStr("whitespace_character", whitespaceCharacter);
    writeInt("artists_limit", artistsLimit);
    writeStr("artists_short_form", artistsShortForm);
    writeStr("key_system", keySystem);
    writeStr("cover_size", coverSize);
    writeBool("keep_cover", keepCover);
    writeBool("fix_tags", fixTags);

    if (!tagMappings.isEmpty()) {
        out << "tag_mappings:\n";
        for (auto fmtIt = tagMappings.constBegin(); fmtIt != tagMappings.constEnd(); ++fmtIt) {
            out << "  " << fmtIt.key() << ":\n";
            for (auto fieldIt = fmtIt.value().constBegin(); fieldIt != fmtIt.value().constEnd(); ++fieldIt)
                out << "    " << fieldIt.key() << ": " << yamlScalar(fieldIt.value()) << '\n';
        }
    }
    writeStr("proxy_mode", proxyMode);
    writeStr("proxy", proxy);
    writeStr("theme", theme);
    writeStr("language", language);
    out.flush();
    f.close();
    return true;
}

void setConfigPathOverride(const QString &path) { g_configOverride = path; }
QString configPathOverride() { return g_configOverride; }

QString findConfigFile(bool *exists)
{
    if (!g_configOverride.isEmpty()) {
        if (exists) *exists = QFileInfo::exists(g_configOverride);
        return g_configOverride;
    }
    return findFile(QStringLiteral("bpdownloader-config.yml"), exists);
}

QString findCacheFile(bool *exists)
{
    if (!g_configOverride.isEmpty()) {
        const QString p = QDir(QFileInfo(g_configOverride).absolutePath())
                              .filePath(QStringLiteral("bpdownloader-credentials.json"));
        if (exists) *exists = QFileInfo::exists(p);
        return p;
    }
    return findFile(QStringLiteral("bpdownloader-credentials.json"), exists);
}

QString findErrorLogFile(bool *exists)
{
    return findFile(QStringLiteral("bpdownloader-err.log"), exists);
}
