#pragma once

#include <QString>
#include <QStringList>
#include <QMap>
#include <QDir>
#include <mutex>
#include <condition_variable>

// Go strings.Fields + " " join：按任意空白拆分并压缩。
QString fieldsJoin(const QStringList &parts);

// 对应 beatport.SanitizedString.UnmarshalJSON：去掉 JSON 文本中残留的
// 字面转义序列（反斜杠+n/r/t，注意不是真实换行符），再做空白压缩。
QString sanitizedStringRaw(const QString &raw);

// SanitizeForPath：删除 '/' 与 '\\'，空白压缩。
QString sanitizeForPath(const QString &s);

// SanitizePath：按字节截断 250、删除 <>:"|?*、可选空白替换、空白压缩。
QString sanitizePath(const QString &name, const QString &whitespace = QString());

// NumberWithPadding：padding==0 时按 total 的位数补零。
QString numberWithPadding(int value, int total, int padding);

// ParseTemplate：{word} 占位替换，未知占位原样保留。
QString parseTemplate(const QString &templ, const QMap<QString, QString> &values);

// Go url.QueryEscape 语义（空格→+，部分子分隔符保留，其余 UTF-8 百分号编码）。
QByteArray queryEscape(const QString &s);

// FNV-1a 64 位小写十六进制（auth.loginId 算法）。
QString fnv64aHex(const QString &s);

// 不带花括号的 UUID 字符串。
QString newUuidString();

QString executableDirPath();
QString executableDirFilePath(const QString &fileName);
QString workingDirFilePath(const QString &fileName);

// findFile：工作目录 → exe 目录（Windows），找到返回 true；
// 找不到时返回最后一个候选路径与 false。
QString findFile(const QString &fileName, bool *exists);

// MkdirAll（0760 仅影响非 Windows；Windows 忽略权限位）。
bool createDirectory(const QString &directory, QString *err);

// 日期 "YYYY-MM-DD" → "YYYY"，无法解析返回空串（Go time.Parse 行为）。
QString yearFromDate(const QString &yyyyMmDd);

// time.Time(RFC3339) → "YYYY-MM-DD"；空/非法返回空串。
QString dateOnlyFromRfc3339(const QString &s);

// sync.WaitGroup 等价物。
class WaitGroup {
public:
    void add(int n = 1) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_counter += n;
    }
    void done() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (--m_counter == 0)
            m_cv.notify_all();
    }
    void wait() {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return m_counter == 0; });
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    int m_counter = 0;
};

// 路径拼接（QDir::cleanPath 风格）。
inline QString joinPath(const QString &a, const QString &b) {
    return QDir(a).filePath(b);
}
