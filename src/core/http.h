#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QUrl>
#include <QList>
#include <QNetworkCookie>
#include <functional>
#include <memory>

struct HttpResult {
    int status = 0;
    QByteArray body;
    QMap<QByteArray, QByteArray> headers;
    QList<QNetworkCookie> cookies;
    QString networkError; // 非空表示传输层失败（超时/连接拒绝等）

    QByteArray header(const QByteArray &name) const;
};

// 代理选路（进程内全局唯一真源，所有出网通道共用）：
//   Off    强制直连（QNetworkProxy::NoProxy，忽略系统代理）；
//   Auto   跟随 Windows 系统代理（QNetworkProxy::DefaultProxy）；
//   Manual 使用 url 指定的代理（http(s):// 或 socks5://，可带账密）。
// setGlobalProxy 后版本号自增，各工作线程下一次请求时自动重配自己的
// thread_local QNetworkAccessManager，因此代理切换即时生效、无需重启。
enum class ProxyMode { Off, Auto, Manual };

struct ProxySetting {
    ProxyMode mode = ProxyMode::Auto;
    QString url; // 仅 Manual 有效
};

void setGlobalProxy(const ProxySetting &setting);
ProxySetting globalProxy();

// "off"/"auto"/"manual" 与枚举互转；非法串 parse 返回 false。
bool parseProxyMode(const QString &s, ProxyMode *out);
QString proxyModeToString(ProxyMode mode);

// BlockingClient：在线程内用同步语义（QEventLoop 阻塞）封装
// QNetworkAccessManager。每个工作线程持有线程局部实例。
//
// 对应 Go beatport.client：40s 超时、不自动跟随 302（OAuth authorize 需要
// 读 Location）。代理策略由全局 ProxySetting 决定。
class BlockingClient {
public:
    explicit BlockingClient(int timeoutMs = 40000);

    // followRedirects=false 时 302 原样返回（读 Location 头）。
    HttpResult request(const QByteArray &method, const QUrl &url,
                       const QMap<QByteArray, QByteArray> &headers,
                       const QByteArray &body,
                       const QByteArray &contentType,
                       bool followRedirects = false);

private:
    int m_timeoutMs;
};

// 流式下载到文件（对应 Go http.Get + io.Copy，自动跟随重定向）。
// 代理策略同样由全局 ProxySetting 决定。
// progress: got/total，total<=0 表示未知长度。失败（含非 200）返回 false。
bool downloadToFile(const QString &url, const QString &destination,
                    const std::function<void(qint64 got, qint64 total)> &progress,
                    QString *err);

// 一次性 GET 整个响应到内存（HLS m3u8 / key / segment 用，跟随重定向）。
HttpResult httpGetFull(const QUrl &url);
