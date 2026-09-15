#pragma once

#include <QString>
#include <QTextStream>
#include <memory>

// 全局日志：Go 版 log 包输出到 stdout（GUI 启动脚本再重定向到
// logs/backend.log）。onefile 无控制台（GUI 子系统），因此直接在内部
// 打开 exe 旁 logs/backend.log，同时可选镜像到控制台（--serve/CLI）。
void loggerInit(const QString &logFilePath, bool mirrorToStdout);

void logInfo(const QString &info);
void logErrorLine(const QString &caller, const QString &err);
