#pragma once

#include <QStringList>

class Application;

// 配置文件缺失时的交互式初始化（问用户名/密码/下载目录/音质 1-4），成功写盘。
bool createConfigInteractively(const QString &configPath, QString *err);

// 命令行批量模式：位置参数是 Beatport 链接或 .txt 文件（逐行读 URL）。
// 返回进程退出码。
int runCliBatch(Application *app, const QStringList &args, bool quiet);

// 交互模式：主提示符（链接/搜索/过滤），退出返回 0。
int runCliInteractive(Application *app, bool quiet);

// CLI 下载进度条（GUI 模式不调用）。
void consoleProgress(const QString &prefix, int percent);
void consoleProgressDone(const QString &prefix);
