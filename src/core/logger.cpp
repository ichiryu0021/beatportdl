#include "logger.h"

#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QMutex>
#include <QMutexLocker>
#include <cstdio>

namespace {
QMutex g_logMutex;
QString g_logFilePath;
bool g_mirrorStdout = false;

void writeLine(const QString &line)
{
    QMutexLocker lock(&g_logMutex);
    if (!g_logFilePath.isEmpty()) {
        QFile f(g_logFilePath);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            QTextStream out(&f);
            out.setEncoding(QStringConverter::Utf8);
            out << line << '\n';
        }
    }
    if (g_mirrorStdout) {
        fputs(line.toLocal8Bit().constData(), stdout);
        fputc('\n', stdout);
        fflush(stdout);
    }
}
}

void loggerInit(const QString &logFilePath, bool mirrorToStdout)
{
    QMutexLocker lock(&g_logMutex);
    g_logFilePath = logFilePath;
    g_mirrorStdout = mirrorToStdout;
    if (!logFilePath.isEmpty()) {
        QFile f(logFilePath);
        if (f.open(QIODevice::Append | QIODevice::Text))
            f.close();
    }
}

static QString timestamp()
{
    // Go log.LstdFlags：2006/01/02 15:04:05
    return QDateTime::currentDateTime().toString(QStringLiteral("yyyy/MM/dd HH:mm:ss"));
}

void logInfo(const QString &info)
{
    writeLine(info);
}

void logErrorLine(const QString &caller, const QString &err)
{
    writeLine(QStringLiteral("%1: %2").arg(caller, err));
}
