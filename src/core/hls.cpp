#include "hls.h"

#include "http.h"
#include "util.h"

#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QUrl>

#include <openssl/evp.h>

namespace {

// Go path.Dir（始终按 '/' 处理，与 Go net/url 拼 base 的行为一致）。
QString goPathDir(const QString &p)
{
    if (p.isEmpty())
        return QStringLiteral(".");
    const int i = p.lastIndexOf(QLatin1Char('/'));
    if (i < 0)
        return QStringLiteral(".");
    if (i == 0)
        return QStringLiteral("/");
    return p.left(i);
}

// 对应 Go: u.Scheme + "://" + u.Host + path.Dir(u.Path) + "/"
QString buildBaseUrl(const QString &playlistUrl, QString *err)
{
    const QUrl u(playlistUrl);
    if (!u.isValid()) {
        if (err)
            *err = u.errorString();
        return QString();
    }

    QString host = u.host(QUrl::FullyDecoded);
    if (u.port() != -1)
        host += QStringLiteral(":%1").arg(u.port());

    return u.scheme() + QStringLiteral("://") + host
           + goPathDir(u.path(QUrl::FullyDecoded)) + QLatin1Char('/');
}

// 从 #EXT-X-KEY 行取属性；quoted=true 对应 URI="..."，否则取裸值。
QString extractAttribute(const QString &line, const char *name, bool quoted)
{
    QRegularExpression re;
    if (quoted)
        re.setPattern(QString::fromLatin1("%1=\"([^\"]*)\"").arg(QString::fromLatin1(name)));
    else
        re.setPattern(QString::fromLatin1("%1=([^,]+)").arg(QString::fromLatin1(name)));
    const QRegularExpressionMatch m = re.match(line);
    return m.hasMatch() ? m.captured(1) : QString();
}

int hexNibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

QByteArray hexDecode(const QString &hexIn, bool *ok)
{
    *ok = false;
    const QByteArray hex = hexIn.toLatin1();
    if ((hex.size() % 2) != 0)
        return {};
    QByteArray out;
    out.reserve(hex.size() / 2);
    for (int i = 0; i < hex.size(); i += 2) {
        const int hi = hexNibble(hex[i]);
        const int lo = hexNibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
            return {};
        out.append(static_cast<char>(hi * 16 + lo));
    }
    *ok = true;
    return out;
}

// QNetworkReply 不暴露 HTTP reason phrase，这里给常见状态码补一份，
// 使错误文本与 Go resp.Status（如 "404 Not Found"）一致。
QString reasonPhrase(int status)
{
    switch (status) {
    case 200: return QStringLiteral("OK");
    case 206: return QStringLiteral("Partial Content");
    case 400: return QStringLiteral("Bad Request");
    case 401: return QStringLiteral("Unauthorized");
    case 403: return QStringLiteral("Forbidden");
    case 404: return QStringLiteral("Not Found");
    case 405: return QStringLiteral("Method Not Allowed");
    case 410: return QStringLiteral("Gone");
    case 416: return QStringLiteral("Range Not Satisfiable");
    case 500: return QStringLiteral("Internal Server Error");
    case 502: return QStringLiteral("Bad Gateway");
    case 503: return QStringLiteral("Service Unavailable");
    case 504: return QStringLiteral("Gateway Timeout");
    default:  return QStringLiteral("Error");
    }
}

} // namespace

bool parseMediaPlaylist(const QString &content, const QString &playlistUrl,
                        HlsPlaylist *out, QString *err)
{
    QString baseErr;
    const QString base = buildBaseUrl(playlistUrl, &baseErr);
    if (base.isEmpty()) {
        if (err)
            *err = baseErr;
        return false;
    }

    HlsPlaylist pl;
    double pendingDuration = 0.0;
    int index = 0;

    const QStringList lines = content.split(QLatin1Char('\n'));
    for (QString raw : lines) {
        const QString line = raw.remove(QLatin1Char('\r')).trimmed();
        if (line.isEmpty())
            continue;

        if (line.startsWith(QLatin1String("#EXT-X-TARGETDURATION:"))) {
            pl.targetDuration = line.mid(22).section(QLatin1Char(','), 0, 0).toDouble();
            continue;
        }
        if (line.startsWith(QLatin1String("#EXT-X-KEY"))) {
            // 仅保留首个 KEY（Go 用 segments[0].Key，不支持密钥轮换）。
            if (!pl.hasKey) {
                pl.hasKey = true;
                pl.keyMethod = extractAttribute(line, "METHOD", false);
                pl.keyUri = extractAttribute(line, "URI", true);
                const QString ivRaw = extractAttribute(line, "IV", false);
                if (!ivRaw.isEmpty()) {
                    // 对应 strings.TrimPrefix(iv, "0x") + hex.DecodeString。
                    QString hex = ivRaw;
                    if (hex.startsWith(QStringLiteral("0x"))
                        || hex.startsWith(QStringLiteral("0X")))
                        hex = hex.mid(2);
                    bool ok = false;
                    pl.keyIv = hexDecode(hex, &ok);
                    if (!ok) {
                        if (err)
                            *err = QStringLiteral("decode stream iv: encoding/hex: invalid byte: %1")
                                       .arg(ivRaw);
                        return false;
                    }
                }
            }
            continue;
        }
        if (line.startsWith(QLatin1String("#EXTINF:"))) {
            pendingDuration = line.mid(8).section(QLatin1Char(','), 0, 0).toDouble();
            continue;
        }
        if (line.startsWith(QLatin1Char('#'))) {
            if (line == QLatin1String("#EXT-X-ENDLIST"))
                break;
            continue;
        }

        // 普通行 = 段地址；Go 直接 base+URI 字符串拼接。
        HlsSegment seg;
        seg.index = index++;
        seg.duration = pendingDuration;
        seg.uri = base + line;
        pl.segments.append(seg);
        pendingDuration = 0.0;
    }

    *out = std::move(pl);
    return true;
}

bool getStreamSegments(const QString &streamUrl, QStringList *segmentUrls,
                       HlsKey *key, QString *err)
{
    const HttpResult resp = httpGetFull(QUrl(streamUrl));
    if (!resp.networkError.isEmpty()) {
        if (err)
            *err = resp.networkError;
        return false;
    }
    if (resp.status != 200) {
        if (err)
            *err = QStringLiteral("request failed with status code: %1").arg(resp.status);
        return false;
    }

    HlsPlaylist pl;
    if (!parseMediaPlaylist(QString::fromUtf8(resp.body), streamUrl, &pl, err))
        return false;

    QString baseErr;
    const QString base = buildBaseUrl(streamUrl, &baseErr);
    if (base.isEmpty()) {
        if (err)
            *err = baseErr;
        return false;
    }

    for (const HlsSegment &seg : pl.segments)
        segmentUrls->append(seg.uri);

    // Go 在 i==0 时取 key；没有段时整块逻辑跳过。
    if (!pl.segments.isEmpty()) {
        if (!pl.hasKey) {
            if (err)
                *err = QStringLiteral("playlist segments are encrypted but no EXT-X-KEY found");
            return false;
        }
        if (pl.keyMethod != QLatin1String("AES-128")) {
            if (err)
                *err = QStringLiteral("unsupported EXT-X-KEY method: %1").arg(pl.keyMethod);
            return false;
        }

        const HttpResult keyResp = httpGetFull(QUrl(base + pl.keyUri));
        if (!keyResp.networkError.isEmpty()) {
            if (err)
                *err = keyResp.networkError;
            return false;
        }
        if (keyResp.status != 200) {
            if (err)
                *err = QStringLiteral("get stream key failed with status code: %1")
                           .arg(keyResp.status);
            return false;
        }

        if (keyResp.body.size() != 16) {
            // Go crypto/aes.NewCipher 的错误文本（这里提前到取 key 处校验）。
            if (err)
                *err = QStringLiteral("crypto/aes: invalid key size %1").arg(keyResp.body.size());
            return false;
        }

        key->value = keyResp.body;
        key->iv = pl.keyIv;
    }

    return true;
}

bool decryptSegment(const QByteArray &ciphertext, const HlsKey &key,
                    QByteArray *plain, QString *err)
{
    if (key.value.size() != 16) {
        if (err)
            *err = QStringLiteral("crypto/aes: invalid key size %1").arg(key.value.size());
        return false;
    }

    // Go 直接用 segment.Key.IV；Beatport 列表恒带 IV。规格要求缺省时用 key 自身作 IV。
    const QByteArray &iv = (key.iv.size() == 16) ? key.iv : key.value;

    constexpr int kBlock = 16;
    const int n = static_cast<int>(ciphertext.size());
    if (n == 0 || (n % kBlock) != 0) {
        if (err)
            *err = QStringLiteral("crypto/cipher: input not full blocks");
        return false;
    }

    QByteArray out(n, Qt::Uninitialized);
    EVP_CIPHER_CTX *ctx = EVP_CIPHER_CTX_new();
    if (!ctx) {
        if (err)
            *err = QStringLiteral("openssl: EVP_CIPHER_CTX_new failed");
        return false;
    }

    bool ok = false;
    int outLen1 = 0;
    int outLen2 = 0;
    const auto *keyPtr = reinterpret_cast<const unsigned char *>(key.value.constData());
    const auto *ivPtr = reinterpret_cast<const unsigned char *>(iv.constData());
    const auto *inPtr = reinterpret_cast<const unsigned char *>(ciphertext.constData());
    auto *outPtr = reinterpret_cast<unsigned char *>(out.data());

    do {
        if (EVP_DecryptInit_ex(ctx, EVP_aes_128_cbc(), nullptr, nullptr, nullptr) != 1)
            break;
        // 与 Go cipher.NewCBCDecrypter 一致：禁用内置 PKCS7，手工按末字节截断。
        EVP_CIPHER_CTX_set_padding(ctx, 0);
        if (EVP_DecryptInit_ex(ctx, nullptr, nullptr, keyPtr, ivPtr) != 1)
            break;
        if (EVP_DecryptUpdate(ctx, outPtr, &outLen1, inPtr, n) != 1)
            break;
        if (EVP_DecryptFinal_ex(ctx, outPtr + outLen1, &outLen2) != 1)
            break;
        ok = true;
    } while (false);

    EVP_CIPHER_CTX_free(ctx);

    if (!ok) {
        if (err)
            *err = QStringLiteral("openssl: AES-128-CBC decrypt failed");
        return false;
    }

    const int total = outLen1 + outLen2;
    out.resize(total);

    // 不校验 padding 合法性，末字节是几就截几（Go: padding := dec[len-1]）。
    const int padding = static_cast<unsigned char>(out[total - 1]);
    if (padding <= total)
        out.chop(padding);

    *plain = std::move(out);
    return true;
}

bool downloadSegments(const QStringList &segmentUrls, const HlsKey &key,
                      const QString &destination,
                      const std::function<void(int done, int total, qint64 bytes)> &progress,
                      QString *err)
{
    const QString dir = QFileInfo(destination).absolutePath();
    QString mkdirErr;
    if (!createDirectory(dir, &mkdirErr)) {
        if (err)
            *err = mkdirErr;
        return false;
    }

    QFile out(destination);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Append)) {
        if (err)
            *err = out.errorString();
        return false;
    }

    const int total = segmentUrls.size();
    qint64 bytesWritten = 0;

    for (int i = 0; i < total; ++i) {
        const HttpResult resp = httpGetFull(QUrl(segmentUrls.at(i)));
        if (!resp.networkError.isEmpty()) {
            if (err)
                *err = resp.networkError;
            out.close();
            return false;
        }
        if (resp.status != 200) {
            // Go: errors.New(resp.Status)；按规格在前面补段 index。
            if (err)
                *err = QStringLiteral("segment %1: %2 %3")
                           .arg(i)
                           .arg(resp.status)
                           .arg(reasonPhrase(resp.status));
            out.close();
            return false;
        }

        QByteArray plain;
        if (!decryptSegment(resp.body, key, &plain, err)) {
            out.close();
            return false;
        }

        const qint64 written = out.write(plain);
        if (written != plain.size()) {
            if (err)
                *err = out.errorString();
            out.close();
            return false;
        }
        bytesWritten += written;

        if (progress)
            progress(i + 1, total, bytesWritten);
    }

    // Qt6 QFileDevice::close() 返回 void；落盘错误由 flush() 暴露
    // （对应 Go file.Close 时未刷盘写入的错误）。
    const bool flushed = out.flush();
    if (!flushed) {
        if (err)
            *err = out.errorString();
        out.close();
        return false;
    }
    out.close();
    return true;
}

bool remuxToM4A(const QString &input, const QString &output, QString *err)
{
    QProcess proc;
    proc.setProgram(QStringLiteral("ffmpeg"));
    proc.setArguments({
        QStringLiteral("-i"), input,
        QStringLiteral("-map_metadata"), QStringLiteral("-1"),
        QStringLiteral("-c:a"), QStringLiteral("copy"),
        output,
    });
    proc.setProcessChannelMode(QProcess::MergedChannels);

    proc.start();
    if (!proc.waitForStarted(-1)) {
        if (err)
            *err = QStringLiteral("ffmpeg: %1").arg(proc.errorString());
        return false;
    }
    proc.waitForFinished(-1);

    if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0) {
        const QString detail = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
        QString base;
        if (proc.exitStatus() != QProcess::NormalExit)
            base = proc.errorString();
        else
            base = QStringLiteral("exit status %1").arg(proc.exitCode()); // 同 Go *ExitError
        if (err)
            *err = QStringLiteral("ffmpeg: %1%2")
                       .arg(base,
                            detail.isEmpty() ? QString() : QStringLiteral("\n") + detail);
        return false;
    }
    return true;
}
