#include "archiver.h"

#include "util.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace {

// 对应 Go copyFile：跨盘 rename 的 copy+flush 回退。
bool copyFile(const QString &src, const QString &dst, QString *err)
{
    QFile in(src);
    if (!in.open(QIODevice::ReadOnly)) {
        if (err)
            *err = in.errorString();
        return false;
    }

    QFile out(dst);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (err)
            *err = out.errorString();
        return false;
    }

    constexpr qint64 kChunk = 1024 * 1024;
    while (!in.atEnd()) {
        const QByteArray chunk = in.read(kChunk);
        if (chunk.isEmpty())
            break;
        if (out.write(chunk) != chunk.size()) {
            if (err)
                *err = out.errorString();
            out.close();
            return false;
        }
    }

    // 对应 Go out.Sync()：先把 Qt 用户态缓冲落盘（Windows fsync 无直接等价）。
    out.flush();
    out.close();
    in.close();
    return true;
}

} // namespace

Archiver::Archiver(QString rootDir, GenreMode mode, QChar whitespace)
    : m_rootDir(std::move(rootDir))
    , m_mode(mode)
    , m_whitespace(whitespace)
{
}

QString Archiver::genreFolder(const Track &track) const
{
    // ModeSubgenreOrGenre：sub_genre 为空回退 genre（Track::subgenreOrGenre）。
    const QString name = (m_mode == GenreMode::SubgenreOrGenre)
                             ? track.subgenreOrGenre()
                             : track.genre.name;
    const QString whitespace = m_whitespace.isNull() ? QString() : QString(m_whitespace);
    return sanitizePath(sanitizeForPath(name), whitespace);
}

QString Archiver::genreDir(const Track &track, QString *err) const
{
    const QString dir = joinPath(m_rootDir, genreFolder(track));
    QString mkdirErr;
    if (!createDirectory(dir, &mkdirErr)) {
        if (err)
            *err = QStringLiteral("create genre directory: %1").arg(mkdirErr);
        return QString();
    }
    return dir;
}

QString Archiver::file(const Track &track, const QString &sourcePath, QString *err) const
{
    // Go 版对 nil track 返回 "nil track"。C++ 侧入参为引用，无法表达空指针；
    // genre 缺失（空串）时与 Go 行为一致：不报错，目录即 root 本身。
    const QString dir = genreDir(track, err);
    if (dir.isEmpty())
        return QString();

    const QString base = QFileInfo(sourcePath).fileName();
    QString dst = joinPath(dir, base);

    // 重名消歧："name.ext" -> "name (1).ext", "name (2).ext", ...
    if (QFileInfo::exists(dst)) {
        const int dot = base.lastIndexOf(QLatin1Char('.'));
        const QString ext = (dot >= 0) ? base.mid(dot) : QString();
        const QString stem = (dot >= 0) ? base.left(dot) : base;
        for (int i = 1; ; ++i) {
            const QString candidate =
                joinPath(dir, QStringLiteral("%1 (%2)%3").arg(stem).arg(i).arg(ext));
            if (!QFileInfo::exists(candidate)) {
                dst = candidate;
                break;
            }
        }
    }

    QFile srcFile(sourcePath);
    if (srcFile.rename(dst))
        return dst;

    // rename 失败（常见于跨卷）：copy + delete 回退，错误文本照 Go。
    const QString renameErr = srcFile.errorString();

    QString copyErr;
    if (!copyFile(sourcePath, dst, &copyErr)) {
        if (err)
            *err = QStringLiteral("move file: rename failed (%1) and copy fallback failed (%2)")
                       .arg(renameErr, copyErr);
        return QString();
    }

    QFile rmSrc(sourcePath);
    if (!rmSrc.remove()) {
        if (err)
            *err = QStringLiteral("move file: copied but failed to remove source: %1")
                       .arg(rmSrc.errorString());
        return dst;
    }
    return dst;
}
