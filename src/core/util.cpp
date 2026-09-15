#include "util.h"

#include <QCoreApplication>
#include <QRegularExpression>
#include <QFileInfo>
#include <QDateTime>
#include <QUuid>
#include <QFile>

static QString collapseFields(const QString &s)
{
    // Go strings.Fields：按 Unicode 空白拆分（Qt 的 \s 等价），单空格连接。
    static const QRegularExpression ws(QStringLiteral("\\s+"));
    QString out = s;
    out.replace(ws, QStringLiteral(" "));
    return out.trimmed();
}

QString fieldsJoin(const QStringList &parts)
{
    return collapseFields(parts.join(' '));
}

QString sanitizedStringRaw(const QString &raw)
{
    // Go replacer 删除的是字面两字符序列 "\n" "\r" "\t"。
    QString out = raw;
    out.remove(QStringLiteral("\\n"));
    out.remove(QStringLiteral("\\r"));
    out.remove(QStringLiteral("\\t"));
    return collapseFields(out);
}

QString sanitizeForPath(const QString &s)
{
    QString out = s;
    out.remove(QLatin1Char('\\'));
    out.remove(QLatin1Char('/'));
    return collapseFields(out);
}

QString sanitizePath(const QString &name, const QString &whitespace)
{
    QString out = name;
    // Go name[:250] 按字节截断。
    const QByteArray utf8 = out.toUtf8();
    if (utf8.size() > 250)
        out = QString::fromUtf8(utf8.left(250));

    for (QChar bad : QStringLiteral("<>:\"|?*"))
        out.remove(bad);

    if (!whitespace.isEmpty())
        out.replace(QLatin1Char(' '), whitespace);

    return collapseFields(out);
}

QString numberWithPadding(int value, int total, int padding)
{
    if (padding == 0)
        padding = QString::number(total).size();
    return QStringLiteral("%1").arg(value, padding, 10, QLatin1Char('0'));
}

QString parseTemplate(const QString &templ, const QMap<QString, QString> &values)
{
    static const QRegularExpression re(QStringLiteral("\\{(\\w+)\\}"));
    QString out;
    int last = 0;
    auto it = re.globalMatch(templ);
    while (it.hasNext()) {
        const auto m = it.next();
        out += templ.mid(last, m.capturedStart() - last);
        const QString key = m.captured(1);
        if (values.contains(key))
            out += values.value(key);
        else
            out += m.captured(0);
        last = m.capturedEnd();
    }
    out += templ.mid(last);
    return out;
}

QByteArray queryEscape(const QString &s)
{
    // 与 Go net/url.QueryEscape（encodeQueryComponent）对齐：
    // 仅字母数字与 -_.~ 不转义；空格→'+'；$&+,/:;=?@ 在 query 组件模式下
    // 同样要转义；其余按 UTF-8 %XX（大写十六进制）。
    static const char unreserved[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.~";

    static const QByteArray hex = "0123456789ABCDEF";
    const QByteArray utf8 = s.toUtf8();
    QByteArray out;
    out.reserve(utf8.size() * 3);
    for (unsigned char c : utf8) {
        if (c == ' ') {
            out.append('+');
        } else if (strchr(unreserved, c) != nullptr) {
            out.append(static_cast<char>(c));
        } else {
            out.append('%');
            out.append(hex[(c >> 4) & 0x0F]);
            out.append(hex[c & 0x0F]);
        }
    }
    return out;
}

QString fnv64aHex(const QString &s)
{
    const QByteArray data = s.toUtf8();
    quint64 hash = 14695981039346656037ULL;
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ULL;
    }
    return QString::fromLatin1(QByteArray::number(hash, 16).rightJustified(16, '0'));
}

QString newUuidString()
{
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString executableDirPath()
{
    return QCoreApplication::applicationDirPath();
}

QString executableDirFilePath(const QString &fileName)
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(fileName);
}

QString workingDirFilePath(const QString &fileName)
{
    return QDir::current().filePath(fileName);
}

QString findFile(const QString &fileName, bool *exists)
{
    const QString working = workingDirFilePath(fileName);
    const QString exec = executableDirFilePath(fileName);

    if (QFileInfo::exists(working)) {
        if (exists) *exists = true;
        return working;
    }
    if (QFileInfo::exists(exec)) {
        if (exists) *exists = true;
        return exec;
    }
    // Windows 上默认路径取 exe 旁（便携化要求：不写系统目录）。
    if (exists) *exists = false;
    return exec;
}

bool createDirectory(const QString &directory, QString *err)
{
    QDir dir(directory);
    if (dir.exists())
        return true;
    if (!dir.mkpath(QStringLiteral("."))) {
        if (err)
            *err = QStringLiteral("create directory: %1").arg(directory);
        return false;
    }
    return true;
}

QString yearFromDate(const QString &yyyyMmDd)
{
    const QDate d = QDate::fromString(yyyyMmDd, QStringLiteral("yyyy-MM-dd"));
    return d.isValid() ? d.toString(QStringLiteral("yyyy")) : QString();
}

QString dateOnlyFromRfc3339(const QString &s)
{
    if (s.size() < 10)
        return {};
    const QDate d = QDate::fromString(s.left(10), QStringLiteral("yyyy-MM-dd"));
    return d.isValid() ? d.toString(QStringLiteral("yyyy-MM-dd")) : QString();
}
