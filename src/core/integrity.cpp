#include "integrity.h"

#include "tags.h"

#include <QFile>
#include <QFileInfo>

// 1:1 对应 Go internal/integrity/integrity.go Checker.Verify。
IntegrityResult Checker::verify(const QString &path, qint64 expectedBytes) const
{
    IntegrityResult result;
    result.filePath = path;
    result.expectedBytes = expectedBytes;

    const QFileInfo info(path);
    if (!info.exists()) {
        // Go: os.Stat 失败时 ExpectedBytes 强制为 -1，Error 为 stat 错误文本。
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            result.error = f.errorString();
        } else {
            f.close();
            result.error = QStringLiteral("stat failed");
        }
        result.expectedBytes = -1;
        return result;
    }

    result.actualBytes = info.size();
    const bool byteOK = expectedBytes < 0 || result.actualBytes == expectedBytes;

    QString tagErr;
    auto file = TaggedFile::read(path, &tagErr);
    if (!file) {
        result.error = QStringLiteral("undecodable: %1").arg(tagErr);
        return result;
    }

    result.sampleRate = file->sampleRate();
    result.decodable = result.sampleRate > 0;
    if (!result.decodable)
        result.error = QStringLiteral("invalid sample rate");

    result.valid = byteOK && result.decodable;
    if (result.valid)
        result.error.clear();
    return result;
}

IntegrityResult Checker::verifyAudio(const QString &path) const
{
    return verify(path, -1);
}
