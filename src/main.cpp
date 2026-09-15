// 程序入口，三模式（对齐 Go BPDL/main.go 的参数拆分）：
//   无参数             → GUI（QApplication + 内嵌 RpcServer + serveLoop 后台线程）
//   --serve            → 内嵌 HTTP/JSON-RPC 服务（QCoreApplication + exec）
//   --cli / 位置参数   → 交互式 / 批量 CLI
//
// Windows 下是 WIN32 子系统：控制台模式先 AttachConsole 挂回父控制台。

#include "core/rpcserver.h"
#include "core/rpc_core.h"
#include "core/cli.h"
#include "core/downloader.h"
#include "core/config.h"
#include "core/logger.h"
#include "main_window.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QMessageBox>
#include <QSharedMemory>
#include <QStringList>
#include <QLocale>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#endif

namespace {

struct Args {
    bool serve = false;
    bool cli = false;
    bool quiet = false;
    QString addr = QStringLiteral("127.0.0.1:8765");
    QString configPath;
    QStringList positional;
};

// 对齐 Go splitEarlyArgs：--serve / --cli / -q 为布尔开关，
// --addr、--config 支持 "--addr X" 与 "--addr=X" 两种写法。
Args parseArgs(int argc, char **argv)
{
    Args a;
    auto takeValue = [&](const QString &tok, const QString &eq, int &i) {
        if (tok.length() > eq.length())
            return tok.mid(eq.length());
        if (i + 1 < argc) {
            ++i;
            return QString::fromLocal8Bit(argv[i]);
        }
        return QString();
    };

    for (int i = 1; i < argc; ++i) {
        const QString tok = QString::fromLocal8Bit(argv[i]);
        if (tok == QLatin1String("--serve")) {
            a.serve = true;
        } else if (tok == QLatin1String("--cli")) {
            a.cli = true;
        } else if (tok == QLatin1String("-q") || tok == QLatin1String("--q")
                   || tok == QLatin1String("--quiet")) {
            a.quiet = true;
        } else if (tok == QLatin1String("--addr") || tok.startsWith(QLatin1String("--addr="))) {
            a.addr = takeValue(tok, QStringLiteral("--addr="), i);
        } else if (tok == QLatin1String("--config")
                   || tok.startsWith(QLatin1String("--config="))) {
            a.configPath = takeValue(tok, QStringLiteral("--config="), i);
        } else {
            a.positional.append(tok);
        }
    }
    return a;
}

// exeDir/logs/backend.log（先建目录）。
QString prepareLogFile()
{
    const QString logDir =
        QCoreApplication::applicationDirPath() + QStringLiteral("/logs");
    QDir().mkpath(logDir);
    return logDir + QStringLiteral("/backend.log");
}

#ifdef _WIN32
// WIN32 子系统默认没有标准流。挂到父控制台；但要尊重父进程的重定向
// （管道/文件句柄保持不动），否则 `exe args > out.log` 抓不到输出。
void attachToConsole()
{
    auto redirected = [](DWORD which) {
        HANDLE h = GetStdHandle(which);
        if (h == nullptr || h == INVALID_HANDLE_VALUE)
            return false;
        const DWORD t = GetFileType(h);
        // FILE_TYPE_PIPE / FILE_TYPE_DISK：被重定向；FILE_TYPE_CHAR：已是控制台。
        return t == FILE_TYPE_PIPE || t == FILE_TYPE_DISK;
    };

    const bool inKeep = redirected(STD_INPUT_HANDLE);
    const bool outKeep = redirected(STD_OUTPUT_HANDLE)
                         || redirected(STD_ERROR_HANDLE);

    if (!inKeep || !outKeep) {
        // 至少有一个流需要控制台：尝试挂父控制台，失败则自建。
        if (AttachConsole(ATTACH_PARENT_PROCESS) == 0)
            AllocConsole();
    }

    FILE *f = nullptr;
    if (!inKeep)
        freopen_s(&f, "CONIN$", "r", stdin);
    if (!redirected(STD_OUTPUT_HANDLE))
        freopen_s(&f, "CONOUT$", "w", stdout);
    if (!redirected(STD_ERROR_HANDLE))
        freopen_s(&f, "CONOUT$", "w", stderr);
}
#endif

// join serveLoop 线程，最多等 seconds 秒，超时 detach（进程随后退出）。
void joinOrDetach(std::thread &t, int seconds)
{
    if (!t.joinable())
        return;
    auto done = std::async(std::launch::async, [&] { t.join(); });
    if (done.wait_for(std::chrono::seconds(seconds)) == std::future_status::timeout)
        t.detach();
}

#ifdef _WIN32
// Go 行为：首次 CTRL 置 stopFlag 优雅退出，第二次直接终止。
Application *g_ctrlApp = nullptr;
RpcServer *g_ctrlServer = nullptr;

BOOL WINAPI consoleCtrlHandler(DWORD)
{
    static std::atomic<int> hits{0};
    if (++hits == 1) {
        if (g_ctrlApp)
            g_ctrlApp->stopFlag = true;
        QMetaObject::invokeMethod(QCoreApplication::instance(), "quit",
                                  Qt::QueuedConnection);
        return TRUE;
    }
    TerminateProcess(GetCurrentProcess(), 1);
    return TRUE; // 不可达
}
#endif

} // namespace

// 控制台模式实现（QCoreApplication 必须用真实 argc/argv 构造一次）。
static int runConsoleImpl(int argc, char **argv, const Args &args)
{
#ifdef _WIN32
    attachToConsole();
#endif

    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("BPDownloader"));

    loggerInit(prepareLogFile(), true);

    if (!args.configPath.isEmpty())
        setConfigPathOverride(args.configPath);

    QString err;
    Application *a =
        Application::setup(args.configPath, args.serve, true /*cliInteractive*/, &err);
    if (!a) {
        std::fprintf(stderr, "setup failed: %s\n", err.toUtf8().constData());
        return 1;
    }

    // --serve：业务核心 + HTTP/SSE 传输层 + serveLoop，事件循环等待退出信号。
    if (args.serve) {
        const int colon = args.addr.lastIndexOf(QLatin1Char(':'));
        const QString host =
            colon >= 0 ? args.addr.left(colon) : QStringLiteral("127.0.0.1");
        quint16 port = colon >= 0 ? static_cast<quint16>(args.addr.mid(colon + 1).toUInt())
                                  : 8765;

        auto *core = new RpcCore(a);
        a->setEventSink(core);
        auto *server = new RpcServer(core);
        QString listenErr;
        if (!server->start(host, port, &listenErr)) {
            std::fprintf(stderr, "%s\n", listenErr.toUtf8().constData());
            delete server;
            delete core;
            return 1;
        }

        std::thread serveThread([a] { a->serveLoop(); });

#ifdef _WIN32
        g_ctrlApp = a;
        g_ctrlServer = server;
        SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
#endif

        const int rc = QCoreApplication::exec();

        a->stopFlag = true;
        server->shutdown();
        joinOrDetach(serveThread, 3);
        delete server;
        delete core;
        return rc;
    }

    // CLI 批量 / 交互。
    if (!args.positional.isEmpty())
        return runCliBatch(a, args.positional, args.quiet);
    return runCliInteractive(a, args.quiet);
}

#ifdef _WIN32
static int runGuiImpl(int argc, char **argv, const Args &args)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("BPDownloader"));
    // Fusion 让 QSS 浅色主题统一作用于复选框/下拉弹层/菜单等控件，
    // 避免不同 Windows 版本原生风格渲染不一致。
    QApplication::setStyle(QStringLiteral("Fusion"));

    // 单实例锁：key 与产品名绑定，旧版本 beatportdl_gui_single_instance 已弃用。
    QSharedMemory singleInstance(QStringLiteral("bpdownloader_gui_single_instance"));
    if (!singleInstance.create(1)) {
        QMessageBox box(QMessageBox::Information,
                        QStringLiteral("BPDownloader"),
                        QObject::tr("The program is already running."),
                        QMessageBox::Ok);
        box.exec();
        return 0;
    }

    // 先套浅色兜底（主窗口构造前），Application::setup 读到配置后再按
    // 持久化的 theme 切换，避免深色用户看到一次浅色闪烁前的无样式瞬间。
    MainWindow::applyApplicationTheme(QStringLiteral("light"));

    // 配置尚未加载，先按系统区域装语言，使单实例提示等早期弹窗可本地化；
    // setup 之后再用配置中的 language 覆盖一次。
    MainWindow::applyApplicationLanguage(
        (QLocale::system().language() == QLocale::Chinese)
            ? QStringLiteral("zh")
            : QStringLiteral("en"));

    loggerInit(prepareLogFile(), false);

    if (!args.configPath.isEmpty())
        setConfigPathOverride(args.configPath);

    QString err;
    Application *a = Application::setup(args.configPath,
                                        true /*serveMode：延迟认证*/,
                                        false /*非交互*/, &err);
    if (!a) {
        QMessageBox::critical(nullptr,
                              QStringLiteral("BPDownloader"),
                              QObject::tr("Initialization failed:\n") + err);
        return 1;
    }

    // GUI 模式：业务核心进程内直连——不创建 HTTP 服务、不监听任何端口，
    // 本机其他进程无法通过 RPC 访问本实例。
    auto *core = new RpcCore(a);
    a->setEventSink(core);

    // 按配置里持久化的主题套 QSS（主窗口显示前生效）。
    MainWindow::applyApplicationTheme(a->getSettings().theme);

    // 按配置安装语言：system 跟随系统区域（中文系统→简体中文），其余显式指定。
    // 必须在 MainWindow 构造前安装，控件创建时的 tr() 才能取到译文。
    {
        QString code = a->getSettings().language;
        if (code == QLatin1String("system"))
            code = (QLocale::system().language() == QLocale::Chinese)
                       ? QStringLiteral("zh")
                       : QStringLiteral("en");
        MainWindow::applyApplicationLanguage(code);
    }

    std::thread serveThread([a] { a->serveLoop(); });

    MainWindow w(core);
    w.show();

    const int rc = app.exec();

    a->stopFlag = true;
    core->shutdown();
    joinOrDetach(serveThread, 2);
    delete core;
    return rc;
}
#endif

int main(int argc, char **argv)
{
    const Args args = parseArgs(argc, argv);

#ifdef _WIN32
    const bool consoleMode = args.serve || args.cli || !args.positional.isEmpty();
    if (consoleMode)
        return runConsoleImpl(argc, argv, args);
    return runGuiImpl(argc, argv, args);
#else
    if (!args.serve && !args.cli && args.positional.isEmpty()) {
        // 非 Windows 的 GUI 模式（理论上不发布，保留可用路径）。
        QApplication app(argc, argv);
        QApplication::setApplicationName(QStringLiteral("BPDownloader"));
        loggerInit(prepareLogFile(), false);
        if (!args.configPath.isEmpty())
            setConfigPathOverride(args.configPath);
        QString err;
        Application *a = Application::setup(args.configPath, true, false, &err);
        if (!a)
            return 1;
        auto *core = new RpcCore(a);
        a->setEventSink(core);
        std::thread t([a] { a->serveLoop(); });
        MainWindow w(core);
        w.show();
        const int rc = app.exec();
        a->stopFlag = true;
        core->shutdown();
        joinOrDetach(t, 2);
        delete core;
        return rc;
    }
    return runConsoleImpl(argc, argv, args);
#endif
}
