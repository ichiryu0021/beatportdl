#include "main_window.h"
#include "backend_client.h"
#include "login_dialog.h"
#include "date_range_widget.h"

#include <QTabWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QListWidget>
#include <QListWidgetItem>
#include <QAbstractItemView>
#include <QScrollBar>
#include <QMouseEvent>
#include <QEvent>
#include <QLineEdit>
#include <QSpinBox>
#include <QComboBox>
#include <QPushButton>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QLabel>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QProgressBar>
#include <QFileDialog>
#include <QDir>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QJsonDocument>
#include <QMessageBox>
#include <QAbstractButton>
#include <QApplication>
#include <QFile>
#include <QDateTime>
#include <QSignalBlocker>
#include <QBrush>
#include <QColor>
#include <QTranslator>
#include <QProcess>
#include <QLocale>

namespace {

// 勾选行高亮：半透明蓝底，让单击/Shift 连选的范围一目了然；
// 取消勾选时还原为样式表默认底色（QBrush() = NoBrush）。
void setRowChecked(QListWidgetItem *it, bool checked)
{
    if (!it) return;
    it->setCheckState(checked ? Qt::Checked : Qt::Unchecked);
    it->setBackground(checked ? QBrush(QColor(72, 128, 214, 72)) : QBrush());
}

QString joinArtists(const QJsonArray &artists)
{
    QStringList names;
    for (const QJsonValue &a : artists)
        names << a.toObject().value("name").toString();
    return names.join(", ");
}

// Format both Beatport track/release objects ({name, mix_name, artists:[{name}],
// genre:{name}, ...}) and search results ({title, artists, genre, ...}).
QString formatResult(const QJsonObject &o)
{
    QString artists;
    const QJsonValue av = o.value("artists");
    if (av.isArray()) artists = joinArtists(av.toArray());
    else artists = av.toString();

    QString name = o.value("name").toString();
    if (name.isEmpty()) name = o.value("title").toString();

    const QString mix = o.value("mix_name").toString();

    QString genre;
    const QJsonValue g = o.value("genre");
    if (g.isObject()) genre = g.toObject().value("name").toString();
    else genre = g.toString();

    QString label = artists;
    if (!name.isEmpty()) {
        if (!label.isEmpty()) label += " - ";
        label += name;
    }
    if (!mix.isEmpty() && mix.compare(name, Qt::CaseInsensitive) != 0)
        label += QStringLiteral(" (%1)").arg(mix);
    if (!genre.isEmpty())
        label += QStringLiteral(" [%1]").arg(genre);
    return label;
}

// 运行环境未安装 Qt 简体中文翻译包，这里统一把消息框标准按钮改为中文文本。
void zhInfo(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox box(QMessageBox::Information, title, text, QMessageBox::Ok, parent);
    if (QAbstractButton *b = box.button(QMessageBox::Ok))
        b->setText(QObject::tr("OK"));
    box.exec();
}

void zhWarning(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    if (QAbstractButton *b = box.button(QMessageBox::Ok))
        b->setText(QObject::tr("OK"));
    box.exec();
}

} // namespace

MainWindow::MainWindow(RpcCore *localCore, QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("BPDownloader"));
    resize(960, 640);

    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);

    m_client = new BackendClient(this);
    if (localCore)
        m_client->attachLocal(localCore);

    buildDiscoverTab();
    buildSearchTab();
    buildSchedulerTab();
    buildDownloadsTab();
    buildSettingsTab();

    // Subscribe to SSE events from the back-end; route them to the Scheduler
    // log and the Downloads table.
    m_client->subscribeEvents([this](const QJsonObject &ev) {
        handleEvent(ev);
    });

    m_client->checkHealth([this](bool ok) {
        onHealthChecked(ok);
    });
    refreshSchedulerStatus();
}

MainWindow::~MainWindow() = default;

void MainWindow::applyApplicationTheme(const QString &theme)
{
    // 两套 QSS 均编译进 qrc；非法/缺失值统一回退浅色，保证不出现无样式窗口。
    const QString key = (theme == QLatin1String("dark"))
                            ? QStringLiteral("dark")
                            : QStringLiteral("light");
    QFile qss(QStringLiteral(":/gui/styles_%1.qss").arg(key));
    if (qss.open(QIODevice::ReadOnly | QIODevice::Text))
        qApp->setStyleSheet(QString::fromUtf8(qss.readAll()));
}

// 翻译器生命周期跟随 qApp；切换语言时先卸载旧的再装新的。
static QTranslator *g_uiTranslator = nullptr;

void MainWindow::applyApplicationLanguage(const QString &code)
{
    if (g_uiTranslator) {
        qApp->removeTranslator(g_uiTranslator);
        delete g_uiTranslator;
        g_uiTranslator = nullptr;
    }
    if (code != QLatin1String("zh"))
        return; // 英文为源码内建语言，无需翻译文件。
    g_uiTranslator = new QTranslator(qApp);
    if (g_uiTranslator->load(
            QStringLiteral(":/i18n/bpdownloader_zh_CN.qm"))) {
        qApp->installTranslator(g_uiTranslator);
    } else {
        delete g_uiTranslator;
        g_uiTranslator = nullptr;
    }
}

void MainWindow::onHealthChecked(bool reachable)
{
    refreshSchedulerStatus();
    loadSettings();
    // 后端可达后立刻查认证状态，未登录则弹登录对话框。
    if (reachable) {
        checkAuth();
    }
}

void MainWindow::setAuthStatusText(const QString &state, const QString &err)
{
    if (!m_settingsStatus)
        return;
    QString text = tr("Auth: %1").arg(state);
    if (!err.isEmpty())
        text += tr(" (%1)").arg(err);
    m_settingsStatus->setText(text);
}

void MainWindow::setLoggedIn(bool loggedIn)
{
    m_loggedIn = loggedIn;
    if (m_logoutBtn)
        m_logoutBtn->setText(loggedIn ? tr("Log Out")
                                      : tr("Log In"));
}

void MainWindow::checkAuth()
{
    m_client->authStatus([this](bool ready, const QString &err) {
        onAuthStatus(ready, err);
    });
}

void MainWindow::onAuthStatus(bool ready, const QString &err)
{
    if (ready) {
        // 已登录（缓存有效或配置含有效凭据），无需动作。
        setLoggedIn(true);
        setAuthStatusText(tr("Logged in"));
        ensureGenresLoaded();
        return;
    }
    // 未认证：弹登录对话框。
    setLoggedIn(false);
    setAuthStatusText(tr("Not logged in"), err);
    showLoginDialog();
}

void MainWindow::showLoginDialog()
{
    auto *dlg = new LoginDialog(m_client, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // 登录成功后整行刷新为"已登录"并加载曲风。
    connect(dlg, &LoginDialog::loggedIn, this, [this]() {
        setLoggedIn(true);
        setAuthStatusText(tr("Logged in"));
        refreshSchedulerStatus();
        ensureGenresLoaded();
    });
    dlg->show();
}

void MainWindow::ensureGenresLoaded()
{
    // 第 0 项固定为"全部曲风"，count()>1 表示 getGenres 已加载过（一次性）。
    // 发现页与定时任务页共用同一套曲风列表。
    if (!m_discoverGenre || m_discoverGenre->count() > 1)
        return;
    m_client->rpc(QStringLiteral("getGenres"), QJsonObject{},
        [this](QJsonValue result) {
            if (!m_discoverGenre) return;
            const QJsonArray arr = result.toArray();
            for (const QJsonValue &v : arr) {
                const QJsonObject o = v.toObject();
                const QString slug = o.value("slug").toString();
                const QString name = o.value("name").toString();
                // Beatport genre_name 参数接受的是曲风显示名（如 "Afro House"），
                // 不是 slug；因此 itemData 存 name。slug 仅用于非空判断。
                if (!slug.isEmpty() && !name.isEmpty()
                    && m_discoverGenre->findData(name) < 0) {
                    m_discoverGenre->addItem(name, name);
                    if (m_schedGenre)
                        m_schedGenre->addItem(name, name);
                }
            }
        },
        [this](QString err) {
            appendSchedulerLog(tr("Failed to load genres: ") + err);
        });
}

void MainWindow::loadSettings()
{
    // getSettings 返回 {downloads_directory, writable, error?, quality, track_exists}：
    // 设置页显示当前目录（不可写则标红并预警一次）并回填音频格式与已存在文件策略。
    m_client->rpc(QStringLiteral("getSettings"), QJsonObject{},
        [this](QJsonValue result) {
            if (!m_settingsDirLabel) return;
            const QJsonObject o = result.toObject();
            const QString path = o.value("downloads_directory").toString();
            const bool writable = o.value("writable").toBool(true);
            m_settingsDirLabel->setText(path.isEmpty() ? tr("(Unknown)") : path);
            if (!writable && !path.isEmpty()) {
                const QString reason = o.value("error").toString();
                m_settingsDirLabel->setStyleSheet(QStringLiteral("color: #c0392b;"));
                warnDownloadsDirectory(reason.isEmpty() ? path : reason);
            }

            // 回填音频格式；回填期间屏蔽 currentIndexChanged，避免误发 setQuality。
            if (m_qualityCombo) {
                const QString q = o.value("quality").toString();
                const int idx = m_qualityCombo->findData(q);
                m_qualityReady = false;
                if (idx >= 0) {
                    m_qualityCombo->setCurrentIndex(idx);
                    m_qualityCombo->setProperty("bpdl_lastIdx", idx);
                }
                m_qualityReady = true;
                m_qualityCombo->setEnabled(true);
            }

            // 回填已存在文件策略（skip/overwrite/update/error），同样屏蔽误发 RPC。
            if (m_trackExistsCombo) {
                const QString mode = o.value("track_exists").toString();
                const int idx = m_trackExistsCombo->findData(mode);
                m_trackExistsReady = false;
                if (idx >= 0) {
                    m_trackExistsCombo->setCurrentIndex(idx);
                    m_trackExistsCombo->setProperty("bpdl_lastIdx", idx);
                }
                m_trackExistsReady = true;
                m_trackExistsCombo->setEnabled(true);
            }

            // 回填文件名命名段（track_file_template），屏蔽期间不发 RPC。
            if (m_nameTitleChk) {
                m_namingReady = false;
                const QString tmpl = o.value("track_file_template").toString();
                applyNameTemplate(tmpl);
                m_nameTitleChk->setProperty("bpdl_lastTmpl", buildNameTemplate());
                m_namingReady = true;
                refreshNamePreview();
            }

            // 回填代理：off=关；auto/manual=开，manual 同时回填地址。
            if (m_proxyEnabledChk) {
                const QString pm = o.value("proxy_mode")
                                       .toString(QStringLiteral("auto"));
                const QString purl = o.value("proxy_url").toString();
                const bool on = pm != QLatin1String("off");
                const QString comboMode =
                    pm == QLatin1String("manual") ? QStringLiteral("manual")
                                                  : QStringLiteral("auto");
                m_proxyReady = false;
                {
                    QSignalBlocker b1(m_proxyEnabledChk);
                    QSignalBlocker b2(m_proxyModeCombo);
                    QSignalBlocker b3(m_proxyEdit);
                    m_proxyEnabledChk->setChecked(on);
                    m_proxyEnabledChk->setEnabled(true);
                    m_proxyModeCombo->setCurrentIndex(
                        m_proxyModeCombo->findData(comboMode));
                    m_proxyEdit->setText(purl);
                }
                m_proxyEnabledChk->setProperty("bpdl_lastMode",
                                               on ? comboMode : QStringLiteral("off"));
                m_proxyEnabledChk->setProperty("bpdl_lastUrl", purl);
                m_proxyReady = true;
                syncProxyControls();
            }

            // 回填界面主题（getSettings 之前 main.cpp 已按配置套过一次 QSS，
            // 这里只同步下拉选项，不重复 apply）。
            if (m_themeCombo) {
                const QString theme = o.value("theme")
                                          .toString(QStringLiteral("light"));
                const int idx = m_themeCombo->findData(theme);
                m_themeReady = false;
                if (idx >= 0)
                    m_themeCombo->setCurrentIndex(idx);
                m_themeCombo->setProperty("bpdl_lastTheme",
                    idx >= 0 ? theme : QStringLiteral("light"));
                m_themeReady = true;
                m_themeCombo->setEnabled(true);
            }

            // 回填语言（main.cpp 已按配置安装翻译，这里只同步下拉）。
            if (m_languageCombo) {
                const QString language = o.value("language")
                                             .toString(QStringLiteral("system"));
                const int idx = m_languageCombo->findData(language);
                m_languageReady = false;
                if (idx >= 0)
                    m_languageCombo->setCurrentIndex(idx);
                m_languageReady = true;
                m_languageCombo->setEnabled(true);
            }

            // 回填曲风分类开关与模式。
            if (m_genreSortChk && m_genreSortCombo) {
                const bool on = o.value("sort_by_genre").toBool(false);
                const QString mode = o.value("genre_sort_mode")
                                         .toString(QStringLiteral("genre"));
                const int idx = m_genreSortCombo->findData(mode);
                m_genreSortReady = false;
                m_genreSortChk->setChecked(on);
                if (idx >= 0)
                    m_genreSortCombo->setCurrentIndex(idx);
                m_genreSortCombo->setEnabled(on);
                m_genreSortChk->setEnabled(true);
                m_genreSortCombo->setEnabled(on);
                m_genreSortReady = true;
            }
        },
        [this](QString) {
            if (m_settingsDirLabel)
                m_settingsDirLabel->setText(tr("(Unknown)"));
        });
}

// 按勾选状态从左到右拼文件名模板：
// [序号 ". "][艺人 " - "]标题[ "(" 版本 ")"]
QString MainWindow::buildNameTemplate() const
{
    QString t;
    if (m_nameNumChk && m_nameNumChk->isChecked())
        t += QStringLiteral("{number}. ");
    if (m_nameArtistsChk && m_nameArtistsChk->isChecked())
        t += QStringLiteral("{artists} - ");
    t += QStringLiteral("{name}");
    if (m_nameMixChk && m_nameMixChk->isChecked())
        t += QStringLiteral(" ({mix_name})");
    return t;
}

// 从持久化的模板反推勾选状态（自定义模板也尽量识别）；标题恒为选中。
void MainWindow::applyNameTemplate(const QString &tmpl)
{
    const QSignalBlocker b1(m_nameNumChk);
    const QSignalBlocker b2(m_nameArtistsChk);
    const QSignalBlocker b3(m_nameMixChk);
    if (!tmpl.isEmpty()) {
        m_nameNumChk->setChecked(tmpl.contains(QStringLiteral("{number}")));
        m_nameArtistsChk->setChecked(tmpl.contains(QStringLiteral("{artists}")));
        m_nameMixChk->setChecked(tmpl.contains(QStringLiteral("{mix_name}")));
    }
    m_nameTitleChk->setChecked(true);
}

// 用占位符替换成示例值，显示最终文件名（统一以 .flac 示例）。
void MainWindow::refreshNamePreview()
{
    if (!m_namePreview) return;
    QString demo = buildNameTemplate();
    demo.replace(QStringLiteral("{number}"), QStringLiteral("01"));
    demo.replace(QStringLiteral("{artists}"), QStringLiteral("Conspiracy 23"));
    demo.replace(QStringLiteral("{name}"), QStringLiteral("Silly Things Intro"));
    demo.replace(QStringLiteral("{mix_name}"), QStringLiteral("Original Mix"));
    m_namePreview->setText(tr("Example: %1.flac").arg(demo));
}

// 开关关闭时模式/地址全禁；自动模式下地址框禁用。
void MainWindow::syncProxyControls()
{
    if (!m_proxyEnabledChk) return;
    const bool on = m_proxyEnabledChk->isChecked();
    m_proxyModeCombo->setEnabled(on);
    m_proxyEdit->setEnabled(
        on && m_proxyModeCombo->currentData().toString() == QLatin1String("manual"));
}

// 按当前控件状态推送 setProxy（off/auto/manual+url）；
// 与上次成功值相同则跳过；失败弹回上一次成功状态并提示。
void MainWindow::pushProxySettings()
{
    const bool on = m_proxyEnabledChk->isChecked();
    const QString mode =
        !on ? QStringLiteral("off") : m_proxyModeCombo->currentData().toString();
    const QString url = m_proxyEdit->text().trimmed();

    if (mode == QLatin1String("manual") && url.isEmpty()) {
        zhWarning(this, tr("Proxy"), tr("Please enter the proxy address, e.g. http://127.0.0.1:7890"));
        return;
    }

    const QString lastMode =
        m_proxyEnabledChk->property("bpdl_lastMode").toString();
    const QString lastUrl = m_proxyEnabledChk->property("bpdl_lastUrl").toString();
    if (mode == lastMode && (mode != QLatin1String("manual") || url == lastUrl))
        return; // 已是上次成功保存的值，不重复发 RPC

    QJsonObject params{{QStringLiteral("mode"), mode}};
    if (mode == QLatin1String("manual"))
        params.insert(QStringLiteral("url"), url);

    m_client->rpc(QStringLiteral("setProxy"), params,
        [this, mode, url](QJsonValue) {
            m_proxyEnabledChk->setProperty("bpdl_lastMode", mode);
            m_proxyEnabledChk->setProperty("bpdl_lastUrl", url);
            if (mode == QLatin1String("off"))
                appendSchedulerLog(tr("Proxy disabled; using a direct connection."));
            else if (mode == QLatin1String("auto"))
                appendSchedulerLog(tr("Proxy set to auto-detect (follow system proxy)."));
            else
                appendSchedulerLog(tr("Proxy set to: %1").arg(url));
        },
        [this, lastMode, lastUrl](QString err) {
            zhWarning(this, tr("Proxy"), tr("Failed to save proxy settings:\n%1").arg(err));
            // 弹回上一次成功状态（屏蔽信号，避免再次触发推送）。
            m_proxyReady = false;
            {
                QSignalBlocker b1(m_proxyEnabledChk);
                QSignalBlocker b2(m_proxyModeCombo);
                QSignalBlocker b3(m_proxyEdit);
                const bool wasOn = lastMode != QLatin1String("off");
                m_proxyEnabledChk->setChecked(wasOn);
                m_proxyModeCombo->setCurrentIndex(m_proxyModeCombo->findData(
                    lastMode == QLatin1String("manual") ? QStringLiteral("manual")
                                                        : QStringLiteral("auto")));
                m_proxyEdit->setText(lastUrl);
            }
            m_proxyReady = true;
            syncProxyControls();
        });
}

void MainWindow::buildDiscoverTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *form = new QFormLayout();
    m_discoverGenre = new QComboBox(page);
    m_discoverGenre->setMinimumWidth(220);
    // 第 0 项"全部曲风"，data 为空字符串；其余项由 getGenres 一次性填入。
    m_discoverGenre->addItem(tr("All Genres"), QStringLiteral(""));
    m_discoverKind = new QComboBox(page);
    m_discoverKind->addItem(tr("Tracks"), QStringLiteral("tracks"));
    m_discoverKind->addItem(tr("Releases"), QStringLiteral("releases"));
    m_discoverDates = new DateRangeWidget(page);

    form->addRow(tr("Genre:"), m_discoverGenre);
    form->addRow(tr("Type:"), m_discoverKind);
    form->addRow(tr("Date:"), m_discoverDates);
    layout->addLayout(form);

    auto *btnRow = new QHBoxLayout();
    auto *fetchBtn = new QPushButton(tr("Fetch"), page);
    auto *selectAllBtn = new QPushButton(tr("Select All"), page);
    auto *clearBtn = new QPushButton(tr("Clear Selection"), page);
    auto *downloadBtn = new QPushButton(tr("Download Selected"), page);
    m_discoverCountLabel = new QLabel(tr("Loaded 0 tracks · 0 selected"), page);
    btnRow->addWidget(fetchBtn);
    btnRow->addWidget(selectAllBtn);
    btnRow->addWidget(clearBtn);
    btnRow->addWidget(downloadBtn);
    btnRow->addSpacing(16);
    btnRow->addWidget(m_discoverCountLabel, 1);
    layout->addLayout(btnRow);

    m_discoverList = new QListWidget(page);
    // 勾选由事件过滤器统一处理（单击整行切换），关闭系统选择模式避免冲突。
    m_discoverList->setSelectionMode(QAbstractItemView::NoSelection);
    m_discoverList->viewport()->installEventFilter(this);
    layout->addWidget(m_discoverList, 1);

    m_tabs->addTab(page, tr("Discover"));

    QObject::connect(fetchBtn, &QPushButton::clicked, this, [this]() {
        ensureGenresLoaded();
        resetDiscoverResults(tr("Fetching…"));
        fetchDiscoverPage(1);
    });
    QObject::connect(selectAllBtn, &QPushButton::clicked, this, [this]() {
        setAllChecked(m_discoverList, true);
    });
    QObject::connect(clearBtn, &QPushButton::clicked, this, [this]() {
        setAllChecked(m_discoverList, false);
    });
    QObject::connect(downloadBtn, &QPushButton::clicked, this, [this]() {
        downloadCheckedItems(m_discoverList);
    });

    // 无限滚动：滚动条接近底部时自动拉下一页。
    QScrollBar *bar = m_discoverList->verticalScrollBar();
    QObject::connect(bar, &QScrollBar::valueChanged, this, [this, bar](int value) {
        if (m_discoverLoading || !m_discoverHasMore)
            return;
        if (value >= bar->maximum() - 4)
            fetchDiscoverPage(m_discoverPage + 1);
    });

    // 初始提示行（与底部状态行共用同一套标记/点击逻辑）。
    setDiscoverStatus(tr("Click \"Fetch\" above to start loading"));
}

void MainWindow::resetDiscoverResults(const QString &statusText)
{
    if (!m_discoverList) return;
    m_discoverList->clear(); // 会删除所有 item（含状态行）
    m_discoverStatusItem = nullptr;
    m_discoverPage = 0;
    m_discoverHasMore = false;
    m_discoverLoading = false;
    m_discoverFailed = false;
    m_discoverLastClickedRow = -1;
    m_discoverSeen.clear();
    setDiscoverStatus(statusText);
    updateDiscoverCount();
}

void MainWindow::setDiscoverStatus(const QString &text)
{
    if (!m_discoverList) return;
    if (!m_discoverStatusItem) {
        m_discoverStatusItem = new QListWidgetItem(text);
        // 状态行：不可勾选/不可选；UserRole+1 用作"状态行"标记。
        m_discoverStatusItem->setFlags(Qt::NoItemFlags);
        m_discoverStatusItem->setData(Qt::UserRole + 1, QStringLiteral("status"));
        m_discoverList->addItem(m_discoverStatusItem);
    } else {
        m_discoverStatusItem->setText(text);
    }
}

void MainWindow::fetchDiscoverPage(int page)
{
    if (!m_discoverList || m_discoverLoading)
        return;
    m_discoverLoading = true;
    m_discoverFailed = false;
    setDiscoverStatus(page == 1 ? tr("Fetching…")
                                : tr("Loading more…"));

    QJsonObject params;
    params.insert("kind", m_discoverKind->currentData().toString());
    params.insert("page", page);
    // 单选曲风 → genre 字符串（显示名，如 "Afro House"）；"全部曲风"（空）不传。
    const QString genreName = m_discoverGenre->currentData().toString();
    if (!genreName.isEmpty())
        params.insert("genre", genreName);
    params.insert("date_from", m_discoverDates->fromIso());
    params.insert("date_to", m_discoverDates->toIso());

    m_client->rpc(QStringLiteral("getLatest"), params,
        [this, page](QJsonValue result) {
            if (!m_discoverList) return;
            m_discoverLoading = false;
            const QJsonObject o = result.toObject();
            appendCheckableResults(m_discoverList, o.value("results").toArray(),
                                   &m_discoverSeen);
            m_discoverPage = o.value("page").toInt(page);
            m_discoverHasMore = o.value("has_more").toBool(false);

            if (m_discoverSeen.isEmpty()) {
                setDiscoverStatus(tr("(No results)"));
            } else if (m_discoverHasMore) {
                setDiscoverStatus(tr("Scroll down to load more (or click this row)"));
            } else {
                setDiscoverStatus(tr("All %1 tracks loaded").arg(m_discoverSeen.size()));
            }
            updateDiscoverCount();
        },
        [this, page](QString err) {
            if (!m_discoverList) return;
            m_discoverLoading = false;
            m_discoverFailed = true;
            if (page == 1 && m_discoverSeen.isEmpty())
                setDiscoverStatus(tr("Fetch failed: %1 (click to retry)").arg(err));
            else
                setDiscoverStatus(tr("Loading failed: %1 (click to retry)").arg(err));
        });
}

void MainWindow::updateDiscoverCount()
{
    if (!m_discoverCountLabel || !m_discoverList) return;
    int selected = 0;
    for (int i = 0; i < m_discoverList->count(); ++i) {
        if (m_discoverList->item(i)->checkState() == Qt::Checked)
            ++selected;
    }
    m_discoverCountLabel->setText(tr("Loaded %1 tracks · %2 selected")
                                      .arg(m_discoverSeen.size()).arg(selected));
}

void MainWindow::buildSearchTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *row = new QHBoxLayout();
    m_searchEdit = new QLineEdit(page);
    m_searchEdit->setPlaceholderText(tr("Search tracks, releases, labels…"));
    auto *searchBtn = new QPushButton(tr("Search"), page);
    auto *selectAllBtn = new QPushButton(tr("Select All"), page);
    auto *clearBtn = new QPushButton(tr("Clear Selection"), page);
    auto *downloadBtn = new QPushButton(tr("Download Selected"), page);
    row->addWidget(m_searchEdit, 1);
    row->addWidget(searchBtn);
    row->addWidget(selectAllBtn);
    row->addWidget(clearBtn);
    row->addWidget(downloadBtn);
    layout->addLayout(row);

    auto *countRow = new QHBoxLayout();
    m_searchCountLabel = new QLabel(tr("Total 0 tracks · 0 selected"), page);
    countRow->addWidget(m_searchCountLabel);
    countRow->addStretch();
    layout->addLayout(countRow);

    m_searchList = new QListWidget(page);
    m_searchList->setSelectionMode(QAbstractItemView::NoSelection);
    m_searchList->viewport()->installEventFilter(this);
    layout->addWidget(m_searchList, 1);

    m_tabs->addTab(page, tr("Search"));

    auto doSearch = [this]() {
        const QString q = m_searchEdit->text().trimmed();
        if (q.isEmpty()) return;
        QJsonObject params;
        params.insert("query", q);

        m_searchList->clear();
        auto *waiting = new QListWidgetItem(tr("Searching…"));
        waiting->setFlags(Qt::NoItemFlags);
        m_searchList->addItem(waiting);

        m_client->rpc(QStringLiteral("search"), params,
            [this](QJsonValue result) {
                if (!m_searchList) return;
                m_searchList->clear();
                const QJsonArray arr = result.toArray();
                QSet<QString> seen; // 搜索页不翻页，仅用于跳过空 URL
                appendCheckableResults(m_searchList, arr, &seen);
                if (m_searchList->count() == 0) {
                    auto *empty = new QListWidgetItem(tr("(No results)"));
                    empty->setFlags(Qt::NoItemFlags);
                    m_searchList->addItem(empty);
                }
                updateListCountLabel(m_searchCountLabel, m_searchList,
                                     tr("Total"));
            },
            [this](QString err) {
                if (!m_searchList) return;
                m_searchList->clear();
                auto *e = new QListWidgetItem(tr("Error: ") + err);
                e->setFlags(Qt::NoItemFlags);
                m_searchList->addItem(e);
            });
    };

    QObject::connect(searchBtn, &QPushButton::clicked, this, doSearch);
    QObject::connect(m_searchEdit, &QLineEdit::returnPressed, this, doSearch);
    QObject::connect(selectAllBtn, &QPushButton::clicked, this, [this]() {
        setAllChecked(m_searchList, true);
    });
    QObject::connect(clearBtn, &QPushButton::clicked, this, [this]() {
        setAllChecked(m_searchList, false);
    });
    QObject::connect(downloadBtn, &QPushButton::clicked, this, [this]() {
        downloadCheckedItems(m_searchList);
    });
}

void MainWindow::buildSchedulerTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *form = new QFormLayout();
    // 网站更新不快，间隔只提供粗粒度预设：一天/一周/一个月/三个月。
    m_schedInterval = new QComboBox(page);
    m_schedInterval->addItem(tr("Daily"), 24 * 60 * 60);
    m_schedInterval->addItem(tr("Weekly"), 7 * 24 * 60 * 60);
    m_schedInterval->addItem(tr("Monthly"), 30 * 24 * 60 * 60);
    m_schedInterval->addItem(tr("Every 3 months"), 90 * 24 * 60 * 60);
    // 曲风选择与"发现最新"页一致：下拉单选，第 0 项"全部曲风"（空=不过滤）。
    m_schedGenre = new QComboBox(page);
    m_schedGenre->setMinimumWidth(220);
    m_schedGenre->addItem(tr("All genres"), QStringLiteral(""));
    form->addRow(tr("Interval:"), m_schedInterval);
    form->addRow(tr("Genre:"), m_schedGenre);
    layout->addLayout(form);

    auto *btnRow = new QHBoxLayout();
    m_schedStart = new QPushButton(tr("Start"), page);
    m_schedStop = new QPushButton(tr("Stop"), page);
    btnRow->addWidget(m_schedStart);
    btnRow->addWidget(m_schedStop);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_schedStatus = new QLabel(tr("Scheduler: unknown"), page);
    layout->addWidget(m_schedStatus);

    m_schedLog = new QPlainTextEdit(page);
    m_schedLog->setReadOnly(true);
    layout->addWidget(m_schedLog, 1);

    m_tabs->addTab(page, tr("Scheduled Tasks"));

    QObject::connect(m_schedStart, &QPushButton::clicked, this, [this]() {
        QJsonObject params;
        params.insert("interval_seconds", m_schedInterval->currentData().toInt());
        const QString genreName = m_schedGenre->currentData().toString();
        if (!genreName.isEmpty())
            params.insert("genre", genreName);

        m_client->rpc(QStringLiteral("startScheduler"), params,
            [this](QJsonValue) {
                appendSchedulerLog(tr("Scheduler started."));
                refreshSchedulerStatus();
            },
            [this](QString err) {
                appendSchedulerLog(tr("Failed to start: ") + err);
            });
    });

    QObject::connect(m_schedStop, &QPushButton::clicked, this, [this]() {
        m_client->rpc(QStringLiteral("stopScheduler"), QJsonObject(),
            [this](QJsonValue) {
                appendSchedulerLog(tr("Scheduler stopped."));
                refreshSchedulerStatus();
            },
            [this](QString err) {
                appendSchedulerLog(tr("Failed to stop: ") + err);
            });
    });

    refreshSchedulerStatus();
}

void MainWindow::buildDownloadsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    m_downloadTable = new QTableWidget(0, 3, page);
    m_downloadTable->setHorizontalHeaderLabels(
        {tr("Track"), tr("Status"), tr("Progress")});
    m_downloadTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_downloadTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_downloadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_downloadTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_downloadTable, 1);

    m_tabs->addTab(page, tr("Downloads"));
}

void MainWindow::buildSettingsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    m_settingsStatus = new QLabel(page);
    m_settingsStatus->setWordWrap(true);
    layout->addWidget(m_settingsStatus);

    // 界面主题：浅色 / 深色，切换即时生效并通过 setTheme 持久化。
    auto *themeRow = new QHBoxLayout;
    m_themeCombo = new QComboBox(page);
    m_themeCombo->addItem(tr("Light"), QStringLiteral("light"));
    m_themeCombo->addItem(tr("Dark"), QStringLiteral("dark"));
    m_themeCombo->setEnabled(false); // 等 getSettings 回填
    themeRow->addWidget(new QLabel(tr("Theme:"), page));
    themeRow->addWidget(m_themeCombo, 1);
    layout->addLayout(themeRow);

    // 界面语言：跟随系统 / English / 简体中文，持久化后重启生效。
    auto *languageRow = new QHBoxLayout;
    m_languageCombo = new QComboBox(page);
    m_languageCombo->addItem(tr("System default"), QStringLiteral("system"));
    m_languageCombo->addItem(QStringLiteral("English"), QStringLiteral("en"));
    m_languageCombo->addItem(tr("Chinese (Simplified)"), QStringLiteral("zh"));
    m_languageCombo->setEnabled(false); // 等 getSettings 回填
    languageRow->addWidget(new QLabel(tr("Language:"), page));
    languageRow->addWidget(m_languageCombo, 1);
    layout->addLayout(languageRow);

    // 曲风分类：勾选后把成品移动到 <下载目录>/<曲风>/ 子文件夹。
    auto *genreSortRow = new QHBoxLayout;
    m_genreSortChk = new QCheckBox(tr("Sort into genre subfolders"), page);
    m_genreSortChk->setEnabled(false);
    m_genreSortCombo = new QComboBox(page);
    m_genreSortCombo->addItem(tr("Genre"), QStringLiteral("genre"));
    m_genreSortCombo->addItem(tr("Subgenre, fallback genre"),
                              QStringLiteral("subgenre_or_genre"));
    m_genreSortCombo->setEnabled(false);
    genreSortRow->addWidget(m_genreSortChk);
    genreSortRow->addWidget(m_genreSortCombo, 1);
    layout->addLayout(genreSortRow);

    // 音频格式（音质）选择：账号支持的下载格式，切换后立即持久化并生效。
    auto *qualityRow = new QHBoxLayout;
    m_qualityCombo = new QComboBox(page);
    m_qualityCombo->addItem(tr("FLAC (Lossless 44.1 kHz)"), QStringLiteral("lossless"));
    m_qualityCombo->addItem(tr("AAC High Quality (256 kbps)"), QStringLiteral("high"));
    m_qualityCombo->addItem(tr("AAC Standard (128 kbps)"), QStringLiteral("medium"));
    m_qualityCombo->addItem(tr("AAC HLS Stream (128 kbps, requires ffmpeg)"),
                            QStringLiteral("medium-hls"));
    m_qualityCombo->setEnabled(false); // 等 getSettings 回填后再启用，避免发空值
    qualityRow->addWidget(new QLabel(tr("Audio Format:"), page));
    qualityRow->addWidget(m_qualityCombo, 1);
    layout->addLayout(qualityRow);

    // 已存在文件策略：skip/overwrite/update/error，切换后立即持久化并生效。
    auto *existsRow = new QHBoxLayout;
    m_trackExistsCombo = new QComboBox(page);
    m_trackExistsCombo->addItem(tr("Skip Download"), QStringLiteral("skip"));
    m_trackExistsCombo->addItem(tr("Overwrite and Re-download"), QStringLiteral("overwrite"));
    m_trackExistsCombo->addItem(tr("Update Tags"), QStringLiteral("update"));
    m_trackExistsCombo->addItem(tr("Fail with Error"), QStringLiteral("error"));
    m_trackExistsCombo->setEnabled(false); // 等 getSettings 回填后再启用
    existsRow->addWidget(new QLabel(tr("Existing Files:"), page));
    existsRow->addWidget(m_trackExistsCombo, 1);
    layout->addLayout(existsRow);

    // 代理行 1：总开关 + 模式（自动跟随系统代理 / 手动指定）。
    auto *proxyRow = new QHBoxLayout;
    m_proxyEnabledChk = new QCheckBox(tr("Use Proxy"), page);
    m_proxyEnabledChk->setEnabled(false); // 等 getSettings 回填
    m_proxyModeCombo = new QComboBox(page);
    m_proxyModeCombo->addItem(tr("Auto-Detect (Follow System Proxy)"), QStringLiteral("auto"));
    m_proxyModeCombo->addItem(tr("Manual"), QStringLiteral("manual"));
    m_proxyModeCombo->setEnabled(false);
    proxyRow->addWidget(m_proxyEnabledChk);
    proxyRow->addWidget(m_proxyModeCombo, 1);
    layout->addLayout(proxyRow);

    // 代理行 2：手动地址（仅"手动设置"时可编辑），支持 http(s)/socks5 与账密。
    auto *proxyUrlRow = new QHBoxLayout;
    m_proxyEdit = new QLineEdit(page);
    m_proxyEdit->setPlaceholderText(tr("http://127.0.0.1:7890 (or socks5://127.0.0.1:7891)"));
    m_proxyEdit->setEnabled(false);
    proxyUrlRow->addWidget(new QLabel(tr("Proxy Address:"), page));
    proxyUrlRow->addWidget(m_proxyEdit, 1);
    layout->addLayout(proxyUrlRow);

    // 文件名命名段：从左到右勾选 序号 / 艺人 / 标题 / 版本，拼成后端模板。
    // 标题（{name}）必选且不可取消——否则不同曲目会同名互相覆盖。
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("File Name:"), page));
    m_nameNumChk = new QCheckBox(tr("Number"), page);
    m_nameArtistsChk = new QCheckBox(tr("Artists"), page);
    m_nameTitleChk = new QCheckBox(tr("Title"), page);
    m_nameMixChk = new QCheckBox(tr("Mix"), page);
    m_nameNumChk->setChecked(true);
    m_nameArtistsChk->setChecked(true);
    m_nameTitleChk->setChecked(true);
    m_nameTitleChk->setEnabled(false);
    m_nameMixChk->setChecked(true);
    nameRow->addWidget(m_nameNumChk);
    nameRow->addWidget(m_nameArtistsChk);
    nameRow->addWidget(m_nameTitleChk);
    nameRow->addWidget(m_nameMixChk);
    nameRow->addStretch(1);
    layout->addLayout(nameRow);

    m_namePreview = new QLabel(page);
    m_namePreview->setStyleSheet(QStringLiteral("color: #555;"));
    m_namePreview->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(m_namePreview);

    // 下载目录行：显示当前目录 + 更改按钮（目录被拒时在此换目录）。
    auto *dirRow = new QHBoxLayout;
    m_settingsDirLabel = new QLabel(tr("(Unknown)"), page);
    m_settingsDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_changeDirBtn = new QPushButton(tr("Change…"), page);
    dirRow->addWidget(new QLabel(tr("Downloads Folder:"), page));
    dirRow->addWidget(m_settingsDirLabel, 1);
    dirRow->addWidget(m_changeDirBtn);
    layout->addLayout(dirRow);

    // 认证按钮行：未登录显示"登录"（点击弹登录框），已登录显示"退出登录"。
    auto *authRow = new QHBoxLayout;
    m_logoutBtn = new QPushButton(tr("Log In"), page);
    authRow->addStretch();
    authRow->addWidget(m_logoutBtn);
    layout->addLayout(authRow);

    layout->addStretch();
    m_tabs->addTab(page, tr("Settings"));

    // 认证状态由 checkAuth 回填；构建期先占位。
    setAuthStatusText(tr("Checking…"));

    QObject::connect(m_changeDirBtn, &QPushButton::clicked, this, [this]() {
        chooseDownloadsDirectory();
    });

    // 音频格式切换：调 setQuality 持久化；失败（如未装 ffmpeg 的 medium-hls）
    // 弹回原值并提示。
    QObject::connect(m_qualityCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            if (!m_qualityReady) return; // 初始回填
            const int prevIdx = m_qualityCombo->property("bpdl_lastIdx").toInt();
            const QString label = m_qualityCombo->currentText();
            const QString q = m_qualityCombo->currentData().toString();
            QJsonObject params;
            params.insert("quality", q);
            m_client->rpc(QStringLiteral("setQuality"), params,
                [this](QJsonValue) {
                    m_qualityCombo->setProperty("bpdl_lastIdx", m_qualityCombo->currentIndex());
                    appendSchedulerLog(tr("Audio format set to: ")
                                       + m_qualityCombo->currentText());
                },
                [this, prevIdx, label](QString err) {
                    zhWarning(this, tr("Audio Format"),
                        tr("Failed to set format \"%1\":\n%2").arg(label, err));
                    QSignalBlocker b(m_qualityCombo);
                    if (prevIdx >= 0)
                        m_qualityCombo->setCurrentIndex(prevIdx);
                });
        });
    m_qualityCombo->setProperty("bpdl_lastIdx", m_qualityCombo->currentIndex());

    // 主题切换：先即时套 QSS 给反馈，再异步 setTheme 持久化；失败回滚选项与样式。
    QObject::connect(m_themeCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            if (!m_themeReady) return; // 初始回填
            const QString prev = m_themeCombo->property("bpdl_lastTheme").toString();
            const QString mode = m_themeCombo->currentData().toString();
            applyApplicationTheme(mode);
            QJsonObject params;
            params.insert("mode", mode);
            m_client->rpc(QStringLiteral("setTheme"), params,
                [this, mode](QJsonValue) {
                    m_themeCombo->setProperty("bpdl_lastTheme", mode);
                },
                [this, prev, mode](QString err) {
                    zhWarning(this, tr("Theme"),
                        tr("Failed to switch theme:\n%1").arg(err));
                    const int prevIdx = m_themeCombo->findData(prev);
                    QSignalBlocker b(m_themeCombo);
                    if (prevIdx >= 0)
                        m_themeCombo->setCurrentIndex(prevIdx);
                    applyApplicationTheme(prev.isEmpty() ? mode : prev);
                });
        });
    m_themeCombo->setProperty("bpdl_lastTheme", QStringLiteral("light"));

    // 语言切换：持久化后需重启才整体生效。确认后退出并以新进程重启自身。
    QObject::connect(m_languageCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            if (!m_languageReady) return; // 初始回填
            const QString mode = m_languageCombo->currentData().toString();
            QJsonObject params;
            params.insert("mode", mode);
            m_client->rpc(QStringLiteral("setLanguage"), params,
                [this, mode](QJsonValue) {
                    const auto answer = QMessageBox::question(
                        this, tr("Language changed"),
                        tr("The language change takes effect after restart. "
                           "Restart now?"),
                        QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
                    if (answer != QMessageBox::Yes)
                        return;
                    // 同路径重启；工作目录保持当前启动目录。
                    QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                            QStringList(),
                                            QDir::currentPath());
                    qApp->quit();
                },
                [this](QString err) {
                    zhWarning(this, tr("Language"),
                        tr("Failed to switch language:\n%1").arg(err));
                });
        });

    // 曲风分类：勾选/模式变化即发 setGenreSorting（后端写盘并重建 Archiver）。
    const auto pushGenreSort = [this]() {
        if (!m_genreSortReady) return;
        const bool enabled = m_genreSortChk->isChecked();
        const QString mode = m_genreSortCombo->currentData().toString();
        m_genreSortCombo->setEnabled(enabled);
        QJsonObject params;
        params.insert("enabled", enabled);
        params.insert("mode", mode);
        m_client->rpc(QStringLiteral("setGenreSorting"), params,
            [](QJsonValue) {},
            [this](QString err) {
                zhWarning(this, tr("Genre folders"),
                    tr("Failed to update genre sorting:\n%1").arg(err));
            });
    };
    QObject::connect(m_genreSortChk, &QCheckBox::toggled, this,
                     [pushGenreSort]() { pushGenreSort(); });
    QObject::connect(m_genreSortCombo,
                     QOverload<int>::of(&QComboBox::currentIndexChanged),
                     this, [pushGenreSort](int) { pushGenreSort(); });

    // 已存在文件策略切换：调 setTrackExists 持久化；失败弹回原选项并提示。
    QObject::connect(m_trackExistsCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            if (!m_trackExistsReady) return; // 初始回填
            const int prevIdx = m_trackExistsCombo->property("bpdl_lastIdx").toInt();
            const QString label = m_trackExistsCombo->currentText();
            const QString mode = m_trackExistsCombo->currentData().toString();
            QJsonObject params;
            params.insert("mode", mode);
            m_client->rpc(QStringLiteral("setTrackExists"), params,
                [this](QJsonValue) {
                    m_trackExistsCombo->setProperty("bpdl_lastIdx",
                                                    m_trackExistsCombo->currentIndex());
                    appendSchedulerLog(tr("Existing file policy set to: ")
                                       + m_trackExistsCombo->currentText());
                },
                [this, prevIdx, label](QString err) {
                    zhWarning(this, tr("Existing Files"),
                        tr("Failed to set existing file policy to \"%1\":\n%2").arg(label, err));
                    QSignalBlocker b(m_trackExistsCombo);
                    if (prevIdx >= 0)
                        m_trackExistsCombo->setCurrentIndex(prevIdx);
                });
        });
    m_trackExistsCombo->setProperty("bpdl_lastIdx", m_trackExistsCombo->currentIndex());

    // 代理：开关/模式即时保存；手动地址在编辑完成（回车/失焦）时保存。
    QObject::connect(m_proxyEnabledChk, &QCheckBox::toggled, this, [this](bool) {
        if (!m_proxyReady) return;
        syncProxyControls();
        pushProxySettings();
    });
    QObject::connect(m_proxyModeCombo,
        QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, [this](int) {
            if (!m_proxyReady) return;
            syncProxyControls();
            // 切到手动但地址为空：等用户填完（editingFinished）再保存。
            if (m_proxyEnabledChk->isChecked()
                && m_proxyModeCombo->currentData().toString()
                       == QLatin1String("manual")
                && m_proxyEdit->text().trimmed().isEmpty())
                return;
            pushProxySettings();
        });
    QObject::connect(m_proxyEdit, &QLineEdit::editingFinished, this, [this]() {
        if (!m_proxyReady) return;
        if (m_proxyEnabledChk->isChecked()
            && m_proxyModeCombo->currentData().toString() == QLatin1String("manual"))
            pushProxySettings();
    });

    // 文件名命名段：任一勾选变化即重算模板、刷新预览并持久化；失败弹回上一次状态。
    m_nameTitleChk->setProperty("bpdl_lastTmpl", buildNameTemplate());
    auto onNamingChanged = [this]() {
        if (!m_namingReady) return;
        refreshNamePreview();
        const QString tmpl = buildNameTemplate();
        QJsonObject params;
        params.insert("template", tmpl);
        m_client->rpc(QStringLiteral("setTrackFileTemplate"), params,
            [this, tmpl](QJsonValue) {
                m_nameTitleChk->setProperty("bpdl_lastTmpl", tmpl);
                appendSchedulerLog(tr("File naming updated."));
            },
            [this](QString err) {
                zhWarning(this, tr("File Name"),
                    tr("Failed to save naming scheme:\n%1").arg(err));
                m_namingReady = false;
                applyNameTemplate(m_nameTitleChk->property("bpdl_lastTmpl").toString());
                m_namingReady = true;
                refreshNamePreview();
            });
    };
    QObject::connect(m_nameNumChk, &QCheckBox::toggled, this, [onNamingChanged](bool) { onNamingChanged(); });
    QObject::connect(m_nameArtistsChk, &QCheckBox::toggled, this, [onNamingChanged](bool) { onNamingChanged(); });
    QObject::connect(m_nameMixChk, &QCheckBox::toggled, this, [onNamingChanged](bool) { onNamingChanged(); });
    refreshNamePreview();

    QObject::connect(m_logoutBtn, &QPushButton::clicked, this, [this]() {
        // 未登录态点击 = 打开登录对话框；已登录态点击 = 退出登录。
        if (!m_loggedIn) {
            showLoginDialog();
            return;
        }
        m_client->logout(
            [this]() {
                setLoggedIn(false);
                setAuthStatusText(tr("Logged out"));
                appendSchedulerLog(tr("Logged out."));
                // 回到登录对话框，重新输入凭据。
                showLoginDialog();
            },
            [this](QString err) {
                zhWarning(this, tr("Log Out"), tr("Log out failed: %1").arg(err));
            });
    });
}

void MainWindow::handleEvent(const QJsonObject &ev)
{
    const QString type = ev.value("event").toString();
    if (type == QStringLiteral("log")) {
        appendSchedulerLog(ev.value("message").toString());
    } else if (type == QStringLiteral("scheduler.found")) {
        const int count = ev.value("count").toInt();
        appendSchedulerLog(tr("Scheduled task found %1 new items.").arg(count));
        refreshSchedulerStatus();
    } else if (type.startsWith(QStringLiteral("download."))) {
        handleDownloadEvent(ev);
    } else if (type == QStringLiteral("auth.loggedOut")) {
        setLoggedIn(false);
        setAuthStatusText(tr("Not logged in"));
    } else {
        appendSchedulerLog(tr("[Event: %1] %2")
            .arg(type, QString::fromUtf8(QJsonDocument(ev).toJson(QJsonDocument::Compact))));
    }
}

void MainWindow::handleDownloadEvent(const QJsonObject &ev)
{
    const QString type = ev.value("event").toString();
    const QString url = ev.value("url").toString();
    QString title = ev.value("title").toString();
    if (title.isEmpty())
        title = url;

    if (type == QStringLiteral("download.started")) {
        ensureDownloadRow(url, title);
        setDownloadRow(url, tr("Downloading"), 0, QString());
    } else if (type == QStringLiteral("download.progress")) {
        ensureDownloadRow(url, title);
        const int percent = ev.value("percent").toInt();
        setDownloadRow(url, tr("Downloading"), percent, QString());
    } else if (type == QStringLiteral("download.done")) {
        ensureDownloadRow(url, title);
        const QString path = ev.value("path").toString();
        setDownloadRow(url, tr("Completed"), 100, path);
    } else if (type == QStringLiteral("download.error")) {
        ensureDownloadRow(url, title);
        const QString err = ev.value("error").toString();
        setDownloadRow(url, tr("Failed"), 0, err);
        // 下载目录不可写（Access denied 等）属于全局问题，预警一次并引导换目录。
        if (err.contains(QStringLiteral("setup downloads directory")) ||
            err.contains(QStringLiteral("Access is denied"))) {
            warnDownloadsDirectory(err);
        }
    } else if (type == QStringLiteral("download.skipped")) {
        // 后端去重/策略跳过：duplicate=已在下载队列中；exists=本地文件已存在。
        ensureDownloadRow(url, title);
        const QString reason = ev.value("reason").toString();
        const QString detail = (reason == QStringLiteral("duplicate"))
            ? tr("Already in download queue")
            : tr("Local file already exists");
        setDownloadRow(url, tr("Skipped"), 0, detail);
    }
}

void MainWindow::appendSchedulerLog(const QString &msg)
{
    if (!m_schedLog) return;
    const QString line = QDateTime::currentDateTime().toString(Qt::ISODate)
                         + QStringLiteral("  ") + msg;
    m_schedLog->appendPlainText(line);
}

void MainWindow::refreshSchedulerStatus()
{
    if (!m_schedStatus) return;
    if (!m_client->isReachable()) {
        m_schedStatus->setText(tr("Scheduler: cannot connect to backend"));
        return;
    }
    m_client->rpc(QStringLiteral("schedulerStatus"), QJsonObject(),
        [this](QJsonValue result) {
            if (!m_schedStatus) return;
            const bool running = result.toObject().value("running").toBool();
            m_schedStatus->setText(running
                ? tr("Scheduler: running")
                : tr("Scheduler: stopped"));
        },
        [this](QString err) {
            if (m_schedStatus)
                m_schedStatus->setText(tr("Scheduler: ") + err);
        });
}

// 向结果列表追加一页带复选框的数据行。seen 非空时做跨页 URL 去重；
// 列表底部的状态行（UserRole+1=="status"）会被临时取出、追加后再放回末尾。
void MainWindow::appendCheckableResults(QListWidget *list, const QJsonArray &arr,
                                        QSet<QString> *seen)
{
    if (!list) return;
    QListWidgetItem *statusItem = nullptr;
    for (int i = 0; i < list->count(); ++i) {
        if (list->item(i)->data(Qt::UserRole + 1).toString()
            == QLatin1String("status")) {
            statusItem = list->takeItem(i);
            break;
        }
    }

    for (const QJsonValue &v : arr) {
        const QJsonObject o = v.toObject();
        const QString url = o.value("url").toString();
        if (url.isEmpty()) continue;
        if (seen) {
            if (seen->contains(url)) continue;
            seen->insert(url);
        }
        auto *item = new QListWidgetItem(formatResult(o));
        item->setData(Qt::UserRole, url);
        item->setToolTip(url);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        setRowChecked(item, false);
        list->addItem(item);
    }

    if (statusItem) list->addItem(statusItem);
}

void MainWindow::updateListCountLabel(QLabel *label, QListWidget *list,
                                      const QString &prefix)
{
    if (!label || !list) return;
    int total = 0, selected = 0;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *it = list->item(i);
        if (it->data(Qt::UserRole + 1).toString() == QLatin1String("status"))
            continue;
        if (it->data(Qt::UserRole).toString().isEmpty())
            continue;
        ++total;
        if (it->checkState() == Qt::Checked)
            ++selected;
    }
    label->setText(tr("%1 %2 tracks · %3 selected").arg(prefix).arg(total).arg(selected));
}

void MainWindow::setAllChecked(QListWidget *list, bool checked)
{
    if (!list) return;
    for (int i = 0; i < list->count(); ++i) {
        QListWidgetItem *it = list->item(i);
        if (it->data(Qt::UserRole).toString().isEmpty())
            continue; // 跳过状态/提示行
        setRowChecked(it, checked);
    }
    if (list == m_discoverList)
        updateDiscoverCount();
    else if (list == m_searchList)
        updateListCountLabel(m_searchCountLabel, m_searchList, tr("Total"));
}

void MainWindow::downloadCheckedItems(QListWidget *list)
{
    QVector<DownloadItem> items;
    for (int i = 0; list && i < list->count(); ++i) {
        QListWidgetItem *it = list->item(i);
        if (it->checkState() != Qt::Checked)
            continue;
        const QString url = it->data(Qt::UserRole).toString();
        if (url.isEmpty())
            continue;
        items.append({url, it->text()});
    }
    if (items.isEmpty()) {
        zhInfo(this, tr("Download"), tr("Select the tracks you want to download first (click a track to toggle, "
                                    "Shift+click for range selection)."));
        return;
    }
    downloadItems(items);
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    QListWidget *list = nullptr;
    int *lastRow = nullptr;
    if (m_discoverList && watched == m_discoverList->viewport()) {
        list = m_discoverList;
        lastRow = &m_discoverLastClickedRow;
    } else if (m_searchList && watched == m_searchList->viewport()) {
        list = m_searchList;
        lastRow = &m_searchLastClickedRow;
    }

    if (list && event->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            QListWidgetItem *item = list->itemAt(me->pos());
            if (item) {
                const bool isStatus =
                    item->data(Qt::UserRole + 1).toString() == QLatin1String("status");
                // 发现页底部状态行：加载更多 / 失败重试。
                if (isStatus && list == m_discoverList) {
                    if (!m_discoverLoading
                        && (m_discoverFailed || m_discoverHasMore)) {
                        fetchDiscoverPage(m_discoverPage + 1);
                    }
                    return true;
                }
                if (!isStatus && !item->data(Qt::UserRole).toString().isEmpty()) {
                    const int row = list->row(item);
                    const bool target = item->checkState() != Qt::Checked;
                    auto setRow = [list](int r, bool checked) {
                        if (r >= 0 && r < list->count())
                            setRowChecked(list->item(r), checked);
                    };
                    if ((me->modifiers() & Qt::ShiftModifier)
                        && *lastRow >= 0 && *lastRow != row) {
                        const int lo = qMin(*lastRow, row);
                        const int hi = qMax(*lastRow, row);
                        for (int r = lo; r <= hi; ++r)
                            setRow(r, target);
                    } else {
                        setRow(row, target);
                    }
                    *lastRow = row;
                    if (list == m_discoverList)
                        updateDiscoverCount();
                    else
                        updateListCountLabel(m_searchCountLabel, m_searchList, tr("Total"));
                    return true; // 吃掉默认行为，避免与复选框双重切换
                }
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::downloadItems(const QVector<DownloadItem> &items)
{
    if (items.isEmpty()) {
        zhInfo(this, tr("Download"), tr("No tracks selected."));
        return;
    }

    // 追加语义：不清空下载表。发送前做客户端去重——下载表中已存在且状态为
    // "排队中/下载中"的 URL 不再发送，只统计被客户端拦截的数量。
    QVector<DownloadItem> toSend;
    int clientBlocked = 0;
    const QString queuedText = tr("Queued");
    const QString downloadingText = tr("Downloading");
    for (const DownloadItem &it : items) {
        const auto rowIt = m_dlRows.constFind(it.url);
        if (rowIt != m_dlRows.constEnd() && m_downloadTable) {
            if (QTableWidgetItem *st = m_downloadTable->item(rowIt.value(), 1)) {
                const QString s = st->text();
                if (s == queuedText || s == downloadingText) {
                    ++clientBlocked;
                    continue;
                }
            }
        }
        toSend.append(it);
    }

    // 预先登记行：排队中。真正开始后由 download.started 事件接管。
    for (const DownloadItem &it : toSend) {
        ensureDownloadRow(it.url, it.title);
        setDownloadRow(it.url, queuedText, 0, QString());
    }

    if (toSend.isEmpty()) {
        appendSchedulerLog(tr("All selected tracks are already in the download queue; %1 blocked by the client.")
                               .arg(clientBlocked));
        return;
    }

    QJsonObject params;
    QJsonArray urlArr;
    for (const DownloadItem &it : toSend) urlArr.append(it.url);
    params.insert("urls", urlArr);

    m_client->rpc(QStringLiteral("download"), params,
        [this, clientBlocked](QJsonValue result) {
            const QJsonObject o = result.toObject();
            const int enqueued = o.value("enqueued").toInt();
            const int skipped = o.value("skipped").toInt();
            appendSchedulerLog(tr("Enqueued %1 tracks, skipped %2 duplicates")
                                   .arg(enqueued).arg(skipped));
            if (clientBlocked > 0)
                appendSchedulerLog(tr("%1 blocked by the client (already queued/downloading).")
                                       .arg(clientBlocked));
        },
        [this](QString err) {
            zhWarning(this, tr("Download"), tr("Error: ") + err);
        });
}

void MainWindow::ensureDownloadRow(const QString &url, const QString &title)
{
    if (!m_downloadTable) return;
    if (m_dlRows.contains(url)) return;
    const int row = m_downloadTable->rowCount();
    m_downloadTable->insertRow(row);

    auto *titleItem = new QTableWidgetItem(title);
    titleItem->setData(Qt::UserRole, url);
    titleItem->setToolTip(url);
    m_downloadTable->setItem(row, 0, titleItem);
    m_downloadTable->setItem(row, 1, new QTableWidgetItem(tr("Queued")));

    auto *bar = new QProgressBar(m_downloadTable);
    bar->setRange(0, 100);
    bar->setValue(0);
    m_downloadTable->setCellWidget(row, 2, bar);

    m_dlRows.insert(url, row);
}

void MainWindow::setDownloadRow(const QString &url, const QString &status,
                                int percent, const QString &detail)
{
    if (!m_downloadTable) return;
    const int row = m_downloadTable->rowCount();
    const int idx = m_dlRows.value(url, -1);
    if (idx < 0 || idx >= row) return;

    QTableWidgetItem *statusItem = m_downloadTable->item(idx, 1);
    if (statusItem) {
        statusItem->setText(status);
        statusItem->setToolTip(detail);
    }
    if (auto *bar = qobject_cast<QProgressBar *>(m_downloadTable->cellWidget(idx, 2))) {
        bar->setValue(percent);
        // 完成后进度条去动态效果，失败标红由 status 列体现。
        if (percent >= 100)
            bar->setFormat(QStringLiteral("%1%").arg(percent));
    }
}

// chooseDownloadsDirectory 弹目录选择窗并调 setDownloadsDirectory RPC。
// 成功后更新设置页显示；失败（Access denied 等）时给出明确提示，引导换目录。
void MainWindow::chooseDownloadsDirectory()
{
    const QString current = m_settingsDirLabel && m_settingsDirLabel->text() != tr("(Unknown)")
        ? m_settingsDirLabel->text()
        : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select Downloads Folder"), current);
    if (dir.isEmpty()) return;

    QJsonObject params;
    params.insert("path", dir);
    m_client->rpc(QStringLiteral("setDownloadsDirectory"), params,
        [this, dir](QJsonValue) {
            if (m_settingsDirLabel) {
                m_settingsDirLabel->setText(dir);
                m_settingsDirLabel->setStyleSheet(QString());
            }
            appendSchedulerLog(tr("Downloads folder set to: ") + dir);
            zhInfo(this, tr("Downloads Folder"), tr("Downloads folder set to:\n%1").arg(dir));
        },
        [this, dir](QString err) {
            zhWarning(this, tr("Downloads Folder"),
                tr("Cannot use %1:\n%2\n\nPlease choose a writable folder.").arg(dir, err));
        });
}

// warnDownloadsDirectory 在下载目录不可写（Access denied 等）时预警一次，
// 并询问是否立即换目录。m_dirWarned 保证同一会话只弹一次，避免刷屏。
void MainWindow::warnDownloadsDirectory(const QString &reason)
{
    if (m_dirWarned) return;
    m_dirWarned = true;
    appendSchedulerLog(tr("Downloads folder is not writable: ") + reason);
    QMessageBox box(QMessageBox::Warning, tr("Downloads Folder"),
        tr("Downloads folder is unusable:\n%1\n\nPlease choose a writable folder. Choose one now?")
            .arg(reason),
        QMessageBox::Yes | QMessageBox::No, this);
    if (auto *b = box.button(QMessageBox::Yes))
        b->setText(tr("Select"));
    if (auto *b = box.button(QMessageBox::No))
        b->setText(tr("Cancel"));
    if (box.exec() == QMessageBox::Yes)
        chooseDownloadsDirectory();
}
