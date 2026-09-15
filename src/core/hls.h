#pragma once

#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QList>
#include <functional>

struct HlsKey {
    QByteArray value;
    QByteArray iv;
};

struct HlsSegment {
    int index = 0;
    double duration = 0;
    QString uri; // 已解析为绝对 URL
};

struct HlsPlaylist {
    double targetDuration = 0;
    QList<HlsSegment> segments;

    // 首个 #EXT-X-KEY（METHOD=AES-128）。
    bool hasKey = false;
    QString keyMethod;
    QString keyUri;       // 属性原值（通常为相对 URI）
    QByteArray keyIv;     // 去掉 0x 前缀后 hex 解码；播放列表未给 IV 时为空
};

// 解析媒体播放列表（首个 #EXT-X-KEY AES-128 的 URI/IV，后续 #EXTINF 段）。
// baseUrl 为播放列表自身 URL，用于拼相对段地址（与 Go base+URI 字符串拼接一致）。
bool parseMediaPlaylist(const QString &content, const QString &playlistUrl,
                        HlsPlaylist *out, QString *err);

// 取流：GET m3u8 → 解析；返回段 URL 列表与 key（key/iv 在首个段时才填充）。
bool getStreamSegments(const QString &streamUrl, QStringList *segmentUrls,
                       HlsKey *key, QString *err);

// AES-128-CBC 解密（禁用内置 padding，按末字节截 PKCS7，与 Go decryptSegment 完全一致）。
bool decryptSegment(const QByteArray &ciphertext, const HlsKey &key,
                    QByteArray *plain, QString *err);

// 逐段 GET→解密→追加写入 destination（目录 0755）。失败已写入文件由调用方清理。
bool downloadSegments(const QStringList &segmentUrls, const HlsKey &key,
                      const QString &destination,
                      const std::function<void(int done, int total, qint64 bytes)> &progress,
                      QString *err);

// ffmpeg -i in -map_metadata -1 -c:a copy out；错误前缀 "ffmpeg: "。
bool remuxToM4A(const QString &input, const QString &output, QString *err);
