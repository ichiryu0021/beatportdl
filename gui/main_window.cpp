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
#include <QDateTime>
#include <QSignalBlocker>
#include <QBrush>
#include <QColor>

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
        b->setText(QStringLiteral("确定"));
    box.exec();
}

void zhWarning(QWidget *parent, const QString &title, const QString &text)
{
    QMessageBox box(QMessageBox::Warning, title, text, QMessageBox::Ok, parent);
    if (QAbstractButton *b = box.button(QMessageBox::Ok))
        b->setText(QStringLiteral("确定"));
    box.exec();
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle(tr("Beatport 下载器"));
    resize(960, 640);

    m_tabs = new QTabWidget(this);
    setCentralWidget(m_tabs);

    m_client = new BackendClient(this);

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

void MainWindow::onHealthChecked(bool reachable)
{
    if (m_settingsStatus) {
        m_settingsStatus->setText(QStringLiteral("后端：%1\n地址：%2")
            .arg(reachable ? QStringLiteral("可达") : QStringLiteral("不可达"))
            .arg(QString::fromLatin1(BackendClient::baseUrl())));
    }
    refreshSchedulerStatus();
    loadSettings();
    // 后端可达后立刻查认证状态，未登录则弹登录对话框。
    if (reachable) {
        checkAuth();
    }
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
        ensureGenresLoaded();
        return;
    }
    // 未认证：弹登录对话框。err 非空时附在后端原因（仅记录，不强制展示）。
    if (m_settingsStatus) {
        m_settingsStatus->setText(m_settingsStatus->text() +
            QStringLiteral("\n认证：未登录") +
            (err.isEmpty() ? QString() : QStringLiteral("（%1）").arg(err)));
    }
    showLoginDialog();
}

void MainWindow::showLoginDialog()
{
    auto *dlg = new LoginDialog(m_client, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    // 登录成功后清掉未登录提示并刷新状态。
    connect(dlg, &LoginDialog::loggedIn, this, [this]() {
        if (m_settingsStatus) {
            // 移除"认证：未登录"提示行
            QString t = m_settingsStatus->text();
            t.remove(QStringLiteral("\n认证：未登录"));
            m_settingsStatus->setText(t);
        }
        refreshSchedulerStatus();
        ensureGenresLoaded();
    });
    dlg->show();
}

void MainWindow::ensureGenresLoaded()
{
    // 第 0 项固定为"全部曲风"，count()>1 表示 getGenres 已加载过（一次性）。
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
                }
            }
        },
        [this](QString err) {
            appendSchedulerLog(tr("加载曲风失败：") + err);
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
            m_settingsDirLabel->setText(path.isEmpty() ? tr("（未知）") : path);
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
        },
        [this](QString) {
            if (m_settingsDirLabel)
                m_settingsDirLabel->setText(tr("（未知）"));
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
    m_namePreview->setText(QStringLiteral("示例：%1.flac").arg(demo));
}

void MainWindow::buildDiscoverTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *form = new QFormLayout();
    m_discoverGenre = new QComboBox(page);
    m_discoverGenre->setMinimumWidth(220);
    // 第 0 项"全部曲风"，data 为空字符串；其余项由 getGenres 一次性填入。
    m_discoverGenre->addItem(tr("全部曲风"), QStringLiteral(""));
    m_discoverKind = new QComboBox(page);
    m_discoverKind->addItem(tr("曲目"), QStringLiteral("tracks"));
    m_discoverKind->addItem(tr("专辑"), QStringLiteral("releases"));
    m_discoverDates = new DateRangeWidget(page);

    form->addRow(QStringLiteral("曲风："), m_discoverGenre);
    form->addRow(QStringLiteral("类型："), m_discoverKind);
    form->addRow(QStringLiteral("日期："), m_discoverDates);
    layout->addLayout(form);

    auto *btnRow = new QHBoxLayout();
    auto *fetchBtn = new QPushButton(QStringLiteral("获取"), page);
    auto *selectAllBtn = new QPushButton(QStringLiteral("全选"), page);
    auto *clearBtn = new QPushButton(QStringLiteral("清空选择"), page);
    auto *downloadBtn = new QPushButton(QStringLiteral("下载所选"), page);
    m_discoverCountLabel = new QLabel(QStringLiteral("已加载 0 首 · 已选 0 首"), page);
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

    m_tabs->addTab(page, QStringLiteral("发现最新"));

    QObject::connect(fetchBtn, &QPushButton::clicked, this, [this]() {
        ensureGenresLoaded();
        resetDiscoverResults(QStringLiteral("获取中…"));
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
    setDiscoverStatus(QStringLiteral("点击上方“获取”开始加载"));
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
    setDiscoverStatus(page == 1 ? QStringLiteral("获取中…")
                                : QStringLiteral("正在加载更多…"));

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
                setDiscoverStatus(QStringLiteral("（无结果）"));
            } else if (m_discoverHasMore) {
                setDiscoverStatus(QStringLiteral("下滑加载更多（或点此行）"));
            } else {
                setDiscoverStatus(QStringLiteral("已加载全部 %1 首").arg(m_discoverSeen.size()));
            }
            updateDiscoverCount();
        },
        [this, page](QString err) {
            if (!m_discoverList) return;
            m_discoverLoading = false;
            m_discoverFailed = true;
            if (page == 1 && m_discoverSeen.isEmpty())
                setDiscoverStatus(QStringLiteral("获取失败：%1（点此重试）").arg(err));
            else
                setDiscoverStatus(QStringLiteral("加载失败：%1（点此重试）").arg(err));
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
    m_discoverCountLabel->setText(tr("已加载 %1 首 · 已选 %2 首")
                                      .arg(m_discoverSeen.size()).arg(selected));
}

void MainWindow::buildSearchTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    auto *row = new QHBoxLayout();
    m_searchEdit = new QLineEdit(page);
    m_searchEdit->setPlaceholderText(QStringLiteral("搜索曲目、专辑、厂牌…"));
    auto *searchBtn = new QPushButton(QStringLiteral("搜索"), page);
    auto *selectAllBtn = new QPushButton(QStringLiteral("全选"), page);
    auto *clearBtn = new QPushButton(QStringLiteral("清空选择"), page);
    auto *downloadBtn = new QPushButton(QStringLiteral("下载所选"), page);
    row->addWidget(m_searchEdit, 1);
    row->addWidget(searchBtn);
    row->addWidget(selectAllBtn);
    row->addWidget(clearBtn);
    row->addWidget(downloadBtn);
    layout->addLayout(row);

    auto *countRow = new QHBoxLayout();
    m_searchCountLabel = new QLabel(QStringLiteral("共 0 首 · 已选 0 首"), page);
    countRow->addWidget(m_searchCountLabel);
    countRow->addStretch();
    layout->addLayout(countRow);

    m_searchList = new QListWidget(page);
    m_searchList->setSelectionMode(QAbstractItemView::NoSelection);
    m_searchList->viewport()->installEventFilter(this);
    layout->addWidget(m_searchList, 1);

    m_tabs->addTab(page, QStringLiteral("搜索"));

    auto doSearch = [this]() {
        const QString q = m_searchEdit->text().trimmed();
        if (q.isEmpty()) return;
        QJsonObject params;
        params.insert("query", q);

        m_searchList->clear();
        auto *waiting = new QListWidgetItem(QStringLiteral("搜索中…"));
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
                    auto *empty = new QListWidgetItem(QStringLiteral("（无结果）"));
                    empty->setFlags(Qt::NoItemFlags);
                    m_searchList->addItem(empty);
                }
                updateListCountLabel(m_searchCountLabel, m_searchList,
                                     QStringLiteral("共"));
            },
            [this](QString err) {
                if (!m_searchList) return;
                m_searchList->clear();
                auto *e = new QListWidgetItem(QStringLiteral("错误：") + err);
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
    m_schedInterval->addItem(QStringLiteral("一天"), 24 * 60 * 60);
    m_schedInterval->addItem(QStringLiteral("一周"), 7 * 24 * 60 * 60);
    m_schedInterval->addItem(QStringLiteral("一个月"), 30 * 24 * 60 * 60);
    m_schedInterval->addItem(QStringLiteral("三个月"), 90 * 24 * 60 * 60);
    m_schedGenre = new QLineEdit(page);
    m_schedGenre->setPlaceholderText(QStringLiteral("曲风名称（可选，如 Afro House）"));
    form->addRow(QStringLiteral("间隔："), m_schedInterval);
    form->addRow(QStringLiteral("曲风（名称，可选）："), m_schedGenre);
    layout->addLayout(form);

    auto *btnRow = new QHBoxLayout();
    m_schedStart = new QPushButton(QStringLiteral("开始"), page);
    m_schedStop = new QPushButton(QStringLiteral("停止"), page);
    btnRow->addWidget(m_schedStart);
    btnRow->addWidget(m_schedStop);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_schedStatus = new QLabel(QStringLiteral("定时任务：未知"), page);
    layout->addWidget(m_schedStatus);

    m_schedLog = new QPlainTextEdit(page);
    m_schedLog->setReadOnly(true);
    layout->addWidget(m_schedLog, 1);

    m_tabs->addTab(page, QStringLiteral("定时任务"));

    QObject::connect(m_schedStart, &QPushButton::clicked, this, [this]() {
        QJsonObject params;
        params.insert("interval_seconds", m_schedInterval->currentData().toInt());
        if (!m_schedGenre->text().trimmed().isEmpty())
            params.insert("genre", m_schedGenre->text().trimmed());

        m_client->rpc(QStringLiteral("startScheduler"), params,
            [this](QJsonValue) {
                appendSchedulerLog(QStringLiteral("定时任务已启动。"));
                refreshSchedulerStatus();
            },
            [this](QString err) {
                appendSchedulerLog(QStringLiteral("启动失败：") + err);
            });
    });

    QObject::connect(m_schedStop, &QPushButton::clicked, this, [this]() {
        m_client->rpc(QStringLiteral("stopScheduler"), QJsonObject(),
            [this](QJsonValue) {
                appendSchedulerLog(QStringLiteral("定时任务已停止。"));
                refreshSchedulerStatus();
            },
            [this](QString err) {
                appendSchedulerLog(QStringLiteral("停止失败：") + err);
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
        {tr("曲目"), tr("状态"), tr("进度")});
    m_downloadTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_downloadTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_downloadTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_downloadTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_downloadTable->verticalHeader()->setVisible(false);
    layout->addWidget(m_downloadTable, 1);

    m_tabs->addTab(page, QStringLiteral("下载"));
}

void MainWindow::buildSettingsTab()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);

    m_settingsStatus = new QLabel(page);
    m_settingsStatus->setWordWrap(true);
    layout->addWidget(m_settingsStatus);

    // 音频格式（音质）选择：账号支持的下载格式，切换后立即持久化并生效。
    auto *qualityRow = new QHBoxLayout;
    m_qualityCombo = new QComboBox(page);
    m_qualityCombo->addItem(tr("FLAC（无损 44.1 kHz）"), QStringLiteral("lossless"));
    m_qualityCombo->addItem(tr("AAC 高品质（256 kbps）"), QStringLiteral("high"));
    m_qualityCombo->addItem(tr("AAC 标准（128 kbps）"), QStringLiteral("medium"));
    m_qualityCombo->addItem(tr("AAC HLS 流（128 kbps，需 ffmpeg）"),
                            QStringLiteral("medium-hls"));
    m_qualityCombo->setEnabled(false); // 等 getSettings 回填后再启用，避免发空值
    qualityRow->addWidget(new QLabel(tr("音频格式："), page));
    qualityRow->addWidget(m_qualityCombo, 1);
    layout->addLayout(qualityRow);

    // 已存在文件策略：skip/overwrite/update/error，切换后立即持久化并生效。
    auto *existsRow = new QHBoxLayout;
    m_trackExistsCombo = new QComboBox(page);
    m_trackExistsCombo->addItem(tr("跳过下载"), QStringLiteral("skip"));
    m_trackExistsCombo->addItem(tr("覆盖重新下载"), QStringLiteral("overwrite"));
    m_trackExistsCombo->addItem(tr("更新标签"), QStringLiteral("update"));
    m_trackExistsCombo->addItem(tr("报错"), QStringLiteral("error"));
    m_trackExistsCombo->setEnabled(false); // 等 getSettings 回填后再启用
    existsRow->addWidget(new QLabel(tr("已存在文件："), page));
    existsRow->addWidget(m_trackExistsCombo, 1);
    layout->addLayout(existsRow);

    // 文件名命名段：从左到右勾选 序号 / 艺人 / 标题 / 版本，拼成后端模板。
    // 标题（{name}）必选且不可取消——否则不同曲目会同名互相覆盖。
    auto *nameRow = new QHBoxLayout;
    nameRow->addWidget(new QLabel(tr("文件名："), page));
    m_nameNumChk = new QCheckBox(tr("序号"), page);
    m_nameArtistsChk = new QCheckBox(tr("艺人"), page);
    m_nameTitleChk = new QCheckBox(tr("标题"), page);
    m_nameMixChk = new QCheckBox(tr("版本"), page);
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
    m_settingsDirLabel = new QLabel(tr("（未知）"), page);
    m_settingsDirLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_changeDirBtn = new QPushButton(tr("更改…"), page);
    dirRow->addWidget(new QLabel(tr("下载目录："), page));
    dirRow->addWidget(m_settingsDirLabel, 1);
    dirRow->addWidget(m_changeDirBtn);
    layout->addLayout(dirRow);

    // 退出登录行。
    auto *authRow = new QHBoxLayout;
    m_logoutBtn = new QPushButton(tr("退出登录"), page);
    authRow->addStretch();
    authRow->addWidget(m_logoutBtn);
    layout->addLayout(authRow);

    auto *hint = new QLabel(
        QStringLiteral("Beatport 账号凭据与下载目录通过 Go 后端的配置文件管理；"
                       "本界面仅通过本地 JSON-RPC 接口（%1）与后端通信。")
            .arg(QString::fromLatin1(BackendClient::baseUrl())), page);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    layout->addStretch();
    m_tabs->addTab(page, QStringLiteral("设置"));

    m_settingsStatus->setText(QStringLiteral("后端：%1\n地址：%2")
        .arg(m_client->isReachable() ? QStringLiteral("可达") : QStringLiteral("检测中…"))
        .arg(QString::fromLatin1(BackendClient::baseUrl())));

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
                    appendSchedulerLog(QStringLiteral("音频格式已设置为：")
                                       + m_qualityCombo->currentText());
                },
                [this, prevIdx, label](QString err) {
                    zhWarning(this, tr("音频格式"),
                        tr("无法设置格式“%1”：\n%2").arg(label, err));
                    QSignalBlocker b(m_qualityCombo);
                    if (prevIdx >= 0)
                        m_qualityCombo->setCurrentIndex(prevIdx);
                });
        });
    m_qualityCombo->setProperty("bpdl_lastIdx", m_qualityCombo->currentIndex());

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
                    appendSchedulerLog(QStringLiteral("已存在文件策略已设置为：")
                                       + m_trackExistsCombo->currentText());
                },
                [this, prevIdx, label](QString err) {
                    zhWarning(this, tr("已存在文件"),
                        tr("无法将已存在文件策略设置为“%1”：\n%2").arg(label, err));
                    QSignalBlocker b(m_trackExistsCombo);
                    if (prevIdx >= 0)
                        m_trackExistsCombo->setCurrentIndex(prevIdx);
                });
        });
    m_trackExistsCombo->setProperty("bpdl_lastIdx", m_trackExistsCombo->currentIndex());

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
                appendSchedulerLog(QStringLiteral("文件名命名方式已更新。"));
            },
            [this](QString err) {
                zhWarning(this, tr("文件名"),
                    tr("无法保存命名方式：\n%1").arg(err));
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
        m_client->logout(
            [this]() {
                if (m_settingsStatus) {
                    m_settingsStatus->setText(m_settingsStatus->text() +
                        QStringLiteral("\n认证：已退出登录"));
                }
                appendSchedulerLog(QStringLiteral("已退出登录。"));
                // 回到登录对话框，重新输入凭据。
                showLoginDialog();
            },
            [this](QString err) {
                zhWarning(this, tr("退出登录"), tr("退出登录失败：%1").arg(err));
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
        appendSchedulerLog(QStringLiteral("定时任务发现 %1 个新项目。").arg(count));
        refreshSchedulerStatus();
    } else if (type.startsWith(QStringLiteral("download."))) {
        handleDownloadEvent(ev);
    } else if (type == QStringLiteral("auth.loggedOut")) {
        if (m_settingsStatus) {
            m_settingsStatus->setText(m_settingsStatus->text() +
                QStringLiteral("\n认证：未登录"));
        }
    } else {
        appendSchedulerLog(QStringLiteral("[事件:%1] %2")
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
        setDownloadRow(url, tr("下载中"), 0, QString());
    } else if (type == QStringLiteral("download.progress")) {
        ensureDownloadRow(url, title);
        const int percent = ev.value("percent").toInt();
        setDownloadRow(url, tr("下载中"), percent, QString());
    } else if (type == QStringLiteral("download.done")) {
        ensureDownloadRow(url, title);
        const QString path = ev.value("path").toString();
        setDownloadRow(url, tr("已完成"), 100, path);
    } else if (type == QStringLiteral("download.error")) {
        ensureDownloadRow(url, title);
        const QString err = ev.value("error").toString();
        setDownloadRow(url, tr("失败"), 0, err);
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
            ? tr("已在下载队列中")
            : tr("本地文件已存在");
        setDownloadRow(url, tr("已跳过"), 0, detail);
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
        m_schedStatus->setText(QStringLiteral("定时任务：无法连接后端"));
        return;
    }
    m_client->rpc(QStringLiteral("schedulerStatus"), QJsonObject(),
        [this](QJsonValue result) {
            if (!m_schedStatus) return;
            const bool running = result.toObject().value("running").toBool();
            m_schedStatus->setText(running
                ? QStringLiteral("定时任务：运行中")
                : QStringLiteral("定时任务：已停止"));
        },
        [this](QString err) {
            if (m_schedStatus)
                m_schedStatus->setText(QStringLiteral("定时任务：") + err);
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
    label->setText(tr("%1 %2 首 · 已选 %3 首").arg(prefix).arg(total).arg(selected));
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
        updateListCountLabel(m_searchCountLabel, m_searchList, tr("共"));
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
        zhInfo(this, tr("下载"), tr("请先勾选要下载的曲目（单击曲目切换勾选，"
                                    "Shift+单击可范围选择）。"));
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
                        updateListCountLabel(m_searchCountLabel, m_searchList, tr("共"));
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
        zhInfo(this, tr("下载"), tr("没有选择任何曲目。"));
        return;
    }

    // 追加语义：不清空下载表。发送前做客户端去重——下载表中已存在且状态为
    // "排队中/下载中"的 URL 不再发送，只统计被客户端拦截的数量。
    QVector<DownloadItem> toSend;
    int clientBlocked = 0;
    const QString queuedText = tr("排队中");
    const QString downloadingText = tr("下载中");
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
        appendSchedulerLog(tr("所选曲目均已在下载队列中，客户端拦截 %1 首。")
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
            appendSchedulerLog(tr("已加入队列 %1 首，跳过重复 %2 首")
                                   .arg(enqueued).arg(skipped));
            if (clientBlocked > 0)
                appendSchedulerLog(tr("客户端拦截 %1 首（已在排队中/下载中）。")
                                       .arg(clientBlocked));
        },
        [this](QString err) {
            zhWarning(this, tr("下载"), tr("错误：") + err);
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
    m_downloadTable->setItem(row, 1, new QTableWidgetItem(tr("排队中")));

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
    const QString current = m_settingsDirLabel && m_settingsDirLabel->text() != tr("（未知）")
        ? m_settingsDirLabel->text()
        : QDir::homePath();
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("选择下载目录"), current);
    if (dir.isEmpty()) return;

    QJsonObject params;
    params.insert("path", dir);
    m_client->rpc(QStringLiteral("setDownloadsDirectory"), params,
        [this, dir](QJsonValue) {
            if (m_settingsDirLabel) {
                m_settingsDirLabel->setText(dir);
                m_settingsDirLabel->setStyleSheet(QString());
            }
            appendSchedulerLog(QStringLiteral("下载目录已设置为：") + dir);
            zhInfo(this, tr("下载目录"), tr("下载目录已设置为：\n%1").arg(dir));
        },
        [this, dir](QString err) {
            zhWarning(this, tr("下载目录"),
                tr("无法使用 %1：\n%2\n\n请选择可写文件夹。").arg(dir, err));
        });
}

// warnDownloadsDirectory 在下载目录不可写（Access denied 等）时预警一次，
// 并询问是否立即换目录。m_dirWarned 保证同一会话只弹一次，避免刷屏。
void MainWindow::warnDownloadsDirectory(const QString &reason)
{
    if (m_dirWarned) return;
    m_dirWarned = true;
    appendSchedulerLog(QStringLiteral("下载目录不可写：") + reason);
    QMessageBox box(QMessageBox::Warning, tr("下载目录"),
        tr("下载目录无法使用：\n%1\n\n请选择可写文件夹，是否立即选择？")
            .arg(reason),
        QMessageBox::Yes | QMessageBox::No, this);
    if (auto *b = box.button(QMessageBox::Yes))
        b->setText(QStringLiteral("选择"));
    if (auto *b = box.button(QMessageBox::No))
        b->setText(QStringLiteral("取消"));
    if (box.exec() == QMessageBox::Yes)
        chooseDownloadsDirectory();
}
