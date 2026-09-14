#include "backend_client.h"
#include "main_window.h"

#include <QApplication>
#include <QMessageBox>
#include <QAbstractButton>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QTimer>
#include <QProcess>
#include <QSharedMemory>
#include <QUrl>
#include <QIODevice>
#include <QStringList>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

// 单次 /health 探测；网络栈/后端正忙时可能失败，因此允许调用方轮询。
bool probeBackend(QNetworkAccessManager *nam, int timeoutMs = 1500)
{
    QNetworkRequest request(QUrl(QString::fromLatin1(BackendClient::baseUrl())
                                 + QStringLiteral("/health")));
    request.setTransferTimeout(timeoutMs);
    QNetworkReply *reply = nam->get(request);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const bool ok = (reply->error() == QNetworkReply::NoError);
    reply->deleteLater();
    return ok;
}

// 在常见位置查找后端可执行文件：部署形态（与 GUI 同目录）优先，
// 其次兼容开发树（gui/build 相对于仓库根的 bin 目录）。
QString locateBackendExe()
{
    const QString dir = QApplication::applicationDirPath();
    const QStringList candidates = {
        dir + QStringLiteral("/beatportdl.exe"),
        dir + QStringLiteral("/../../bin/beatportdl.exe"),
        dir + QStringLiteral("/beatportdl"),
    };
    for (const QString &c : candidates) {
        const QString abs = QDir::cleanPath(c);
        if (QFileInfo::exists(abs))
            return abs;
    }
    return QString();
}

// 以后台子进程方式无窗口启动 beatportdl --serve：
// - CREATE_NO_WINDOW：不弹出黑色控制台；
// - stdout/stderr 重定向到 logs/backend.log，保留可观测性；
// 返回的 QProcess 由调用方保持存活；GUI 退出时 kill 回收。
QProcess *startBackendHidden(const QString &exePath, QString *logPathOut = nullptr)
{
    const QString dir = QApplication::applicationDirPath();
    QDir().mkpath(dir + QStringLiteral("/logs"));
    const QString logPath = dir + QStringLiteral("/logs/backend.log");
    if (logPathOut)
        *logPathOut = logPath;

    auto *proc = new QProcess(qApp);
    proc->setProgram(exePath);
    proc->setArguments({QStringLiteral("--serve")});
    proc->setWorkingDirectory(QFileInfo(exePath).absolutePath());
    proc->setProcessChannelMode(QProcess::MergedChannels);
    proc->setStandardOutputFile(logPath, QIODevice::Append | QIODevice::Text);

#ifdef Q_OS_WIN
    // 不创建/继承控制台窗口；不设 CREATE_NEW_CONSOLE。
    proc->setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments *args) {
            args->flags |= CREATE_NO_WINDOW;
        });
#endif

    // 在日志里分隔每次启动，便于排查。
    if (QFile f(logPath); f.open(QIODevice::Append | QIODevice::Text)) {
        f.write(QStringLiteral("\n===== GUI 启动后端 %1 =====\n")
                    .arg(QDateTime::currentDateTime().toString(Qt::ISODate))
                    .toUtf8());
    }

    proc->start();
    return proc;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("Beatport 下载器"));

    // 单实例保护：命名共享内存被占用说明已有一个 GUI 在运行，直接退出，
    // 避免重复双击/启动脚本产生多个窗口。
    static QSharedMemory singleInstance(QStringLiteral("beatportdl_gui_single_instance"));
    if (!singleInstance.create(1)) {
        QMessageBox box(QMessageBox::Warning, QApplication::applicationName(),
            QStringLiteral("程序已经在运行中，请勿重复打开。"),
            QMessageBox::Ok);
        if (QAbstractButton *b = box.button(QMessageBox::Ok))
            b->setText(QStringLiteral("确定"));
        box.exec();
        return 0;
    }

    // Load the optional stylesheet shipped next to the binary, falling back
    // to the current working directory.
    const QStringList qssCandidates = {
        QApplication::applicationDirPath() + QStringLiteral("/styles.qss"),
        QStringLiteral("styles.qss"),
    };
    for (const QString &candidate : qssCandidates) {
        QFile qss(candidate);
        if (qss.open(QIODevice::ReadOnly | QIODevice::Text)) {
            app.setStyleSheet(QString::fromUtf8(qss.readAll()));
            break;
        }
    }

    // 后端连接策略：先探测；不通则自动在后台无窗口拉起同目录的
    // beatportdl --serve，并把输出写入 logs/backend.log，然后轮询等待就绪。
    QNetworkAccessManager nam;
    bool reachable = probeBackend(&nam);

    QProcess *backendProc = nullptr;
    QString backendLogPath;
    if (!reachable) {
        const QString exePath = locateBackendExe();
        if (!exePath.isEmpty()) {
            backendProc = startBackendHidden(exePath, &backendLogPath);

            // 轮询最多约 15 秒（本地进程，正常 1-3 秒内就绪）。
            QEventLoop waitLoop;
            QTimer timer;
            timer.setInterval(500);
            int attempts = 0;
            QObject::connect(&timer, &QTimer::timeout, &waitLoop, [&] {
                ++attempts;
                if (probeBackend(&nam, 1200) || attempts >= 30)
                    waitLoop.quit();
            });
            // 后端提前退出（如端口被占/配置错误）则立即结束等待。
            QObject::connect(backendProc,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                &waitLoop, &QEventLoop::quit);
            timer.start();
            waitLoop.exec();
            timer.stop();
            reachable = probeBackend(&nam);
        }
    }

    if (!reachable) {
        QMessageBox box(QMessageBox::Warning, QApplication::applicationName(),
            QStringLiteral("无法连接位于 %1 的后端，自动启动也未成功。\n\n"
                           "可手动运行：beatportdl --serve\n"
                           "后端日志：%2\n\n"
                           "界面仍会打开，但在后端运行之前，大多数操作将无法使用。")
                .arg(QString::fromLatin1(BackendClient::baseUrl()),
                     backendLogPath.isEmpty() ? QStringLiteral("（未生成）") : backendLogPath),
            QMessageBox::Ok);
        if (QAbstractButton *b = box.button(QMessageBox::Ok))
            b->setText(QStringLiteral("确定"));
        box.exec();
    }

    // 仅回收由 GUI 自己拉起的后端；用户手动启动的后端不受影响。
    if (backendProc) {
        QObject::connect(&app, &QCoreApplication::aboutToQuit, [backendProc] {
            if (backendProc->state() != QProcess::NotRunning) {
                backendProc->kill();
                backendProc->waitForFinished(3000);
            }
            backendProc->deleteLater();
        });
    }

    MainWindow w;
    w.show();
    return app.exec();
}
