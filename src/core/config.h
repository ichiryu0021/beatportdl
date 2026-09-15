#pragma once

#include <QString>
#include <QMap>
#include <QSharedPointer>

struct AppConfig {
    QString username;
    QString password;
    QString quality = QStringLiteral("lossless");
    bool writeErrorLog = false;
    bool showProgress = true;

    int maxGlobalWorkers = 15;
    int maxDownloadWorkers = 15;

    QString downloadsDirectory;
    bool sortByContext = false;
    bool sortByLabel = false;
    bool forceReleaseDirectories = false;
    bool sortByGenre = false;
    QString genreSortMode = QStringLiteral("genre");
    bool verifyIntegrity = true;
    QString trackExists = QStringLiteral("update");
    int trackNumberPadding = 2;

    QString releaseDirectoryTemplate;
    QString playlistDirectoryTemplate;
    QString chartDirectoryTemplate;
    QString labelDirectoryTemplate;
    QString artistDirectoryTemplate;
    QString trackFileTemplate;
    QString whitespaceCharacter;
    int artistsLimit = 3;
    QString artistsShortForm;
    QString keySystem;

    QString coverSize;
    bool keepCover = false;
    bool fixTags = true;

    // format("flac"/"m4a") -> field -> tag property
    QMap<QString, QMap<QString, QString>> tagMappings;

    // 代理：proxyMode = "off"（强制直连）/ "auto"（跟随系统代理）/
    // "manual"（手动 proxy URL）。proxy 仅 manual 时使用，保留旧键名以
    // 兼容旧版本配置文件（旧文件 proxy 非空即视为 manual）。
    QString proxyMode = QStringLiteral("auto");
    QString proxy;

    // GUI 主题："light"（浅色，默认）/ "dark"（深色）。
    QString theme = QStringLiteral("light");

    // GUI 语言："system"（跟随系统，默认）/ "en" / "zh"。
    QString language = QStringLiteral("system");

    void applyDefaults();
    bool save(const QString &filePath, QString *err) const;
};

extern const QString kDefaultCoverSize;
extern const QStringList kSupportedTrackExistsOptions;
extern const QStringList kSupportedKeySystems;

bool ffmpegInstalled();

QStringList supportedTagMappingFields();
QMap<QString, QMap<QString, QString>> defaultTagMappings();
// 空错误串表示通过。
QString validateTagMappings(const QMap<QString, QMap<QString, QString>> &m);

// lenient=false：强制 username/password 非空（CLI）；true：--serve/GUI 延迟认证。
QSharedPointer<AppConfig> parseConfigFile(const QString &filePath, bool lenient, QString *err);

// --config 覆盖；空串表示走默认搜索顺序（cwd → exe 旁）。
void setConfigPathOverride(const QString &path);
QString configPathOverride();

QString findConfigFile(bool *exists);
QString findCacheFile(bool *exists);
QString findErrorLogFile(bool *exists);
