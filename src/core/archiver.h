#pragma once

#include "models.h"
#include <QString>
#include <memory>

enum class GenreMode {
    Genre,
    SubgenreOrGenre,
};

// 按曲风把下载好的文件归档进子目录（对应 Go internal/sorting/genre.go）。
// 目录名经 SanitizeForPath + SanitizePath(whitespace) 消毒；
// 目标重名时在扩展名前加 " (n)"；跨盘 rename 失败则 copy+delete 回退。
class Archiver {
public:
    Archiver(QString rootDir, GenreMode mode, QChar whitespace);

    QString genreFolder(const Track &track) const;
    QString genreDir(const Track &track, QString *err) const;

    // File：把 sourcePath 移入对应曲风目录，返回新路径。
    QString file(const Track &track, const QString &sourcePath, QString *err) const;

    QString rootDir() const { return m_rootDir; }

private:
    QString m_rootDir;
    GenreMode m_mode;
    QChar m_whitespace;
};
