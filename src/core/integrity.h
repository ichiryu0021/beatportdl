#pragma once

#include <QString>

struct IntegrityResult {
    bool valid = false;
    QString filePath;
    qint64 expectedBytes = -1;
    qint64 actualBytes = 0;
    bool decodable = false;
    int sampleRate = 0;
    QString error;
};

// Verify：文件大小（expectedBytes>=0 时校验）+ TagLib 可解码 + 采样率有效。
class Checker {
public:
    IntegrityResult verify(const QString &path, qint64 expectedBytes = -1) const;
    IntegrityResult verifyAudio(const QString &path) const;
};
