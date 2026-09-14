#pragma once

#include <QMainWindow>
#include <QString>
#include <QStringList>
#include <QJsonObject>
#include <QHash>
#include <QSet>
#include <QVector>

class BackendClient;
class DateRangeWidget;
class QTabWidget;
class QListWidget;
class QListWidgetItem;
class QLineEdit;
class QSpinBox;
class QComboBox;
class QPushButton;
class QCheckBox;
class QPlainTextEdit;
class QLabel;
class QTableWidget;

// DownloadItem 描述一个可下载结果（标题 + 商店页 URL）。
struct DownloadItem
{
    QString url;
    QString title;
};

class MainWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

private:
    void onHealthChecked(bool reachable);
    void checkAuth(); // 启动后查 authStatus，未认证则弹登录对话框
    void onAuthStatus(bool ready, const QString &err);
    void showLoginDialog();
    void ensureGenresLoaded(); // 拉取官网曲风列表填入单选下拉（登录后/抓取时），只加载一次
    void loadSettings(); // 设置页：当前下载目录（含可写性）+ 音频格式

    BackendClient *m_client = nullptr;
    QTabWidget *m_tabs = nullptr;

    // Discover/Latest tab
    QComboBox *m_discoverGenre = nullptr;
    QComboBox *m_discoverKind = nullptr;
    DateRangeWidget *m_discoverDates = nullptr;
    QListWidget *m_discoverList = nullptr;
    QLabel *m_discoverCountLabel = nullptr;
    // 无限滚动状态：m_discoverPage 已加载到第几页；m_discoverSeen 跨页 URL 去重；
    // m_discoverStatusItem 为列表底部状态行（加载中/点此加载更多/已加载全部/失败重试）。
    int m_discoverPage = 0;
    bool m_discoverHasMore = false;
    bool m_discoverLoading = false;
    bool m_discoverFailed = false;
    QSet<QString> m_discoverSeen;
    QListWidgetItem *m_discoverStatusItem = nullptr;
    int m_discoverLastClickedRow = -1;

    // Search tab
    QLineEdit *m_searchEdit = nullptr;
    QListWidget *m_searchList = nullptr;
    QLabel *m_searchCountLabel = nullptr;
    int m_searchLastClickedRow = -1;

    // Scheduler tab
    QComboBox *m_schedInterval = nullptr;
    QLineEdit *m_schedGenre = nullptr;
    QPushButton *m_schedStart = nullptr;
    QPushButton *m_schedStop = nullptr;
    QLabel *m_schedStatus = nullptr;
    QPlainTextEdit *m_schedLog = nullptr;

    // Downloads tab: 列 = 曲目 / 状态 / 进度；行按 url 定位。
    QTableWidget *m_downloadTable = nullptr;
    QHash<QString, int> m_dlRows; // url -> row index

    // Settings tab
    QLabel *m_settingsStatus = nullptr;
    QLabel *m_settingsDirLabel = nullptr;
    QPushButton *m_changeDirBtn = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    bool m_qualityReady = false; // 初始回填前不触发 setQuality RPC
    QComboBox *m_trackExistsCombo = nullptr;
    bool m_trackExistsReady = false; // 初始回填前不触发 setTrackExists RPC
    // 文件名命名段勾选：序号 / 艺人 / 标题(必选) / 版本；按勾选从左到右拼模板。
    QCheckBox *m_nameNumChk = nullptr;
    QCheckBox *m_nameArtistsChk = nullptr;
    QCheckBox *m_nameTitleChk = nullptr;
    QCheckBox *m_nameMixChk = nullptr;
    QLabel *m_namePreview = nullptr;
    bool m_namingReady = false; // 初始回填前不触发 setTrackFileTemplate RPC
    QPushButton *m_logoutBtn = nullptr;

    void buildDiscoverTab();
    void buildSearchTab();
    void buildSchedulerTab();
    void buildDownloadsTab();
    void buildSettingsTab();

    void appendSchedulerLog(const QString &msg);
    void handleEvent(const QJsonObject &ev);
    void handleDownloadEvent(const QJsonObject &ev);
    void refreshSchedulerStatus();

    // 发现页无限滚动：重置结果集/请求指定页/更新底部状态行与计数。
    void resetDiscoverResults(const QString &statusText);
    void fetchDiscoverPage(int page);
    void setDiscoverStatus(const QString &text);
    void updateDiscoverCount();
    // 搜索/发现列表共用：复选框行、计数、全选/清空、收集勾选项。
    void appendCheckableResults(QListWidget *list, const QJsonArray &arr, QSet<QString> *seen);
    void updateListCountLabel(QLabel *label, QListWidget *list, const QString &prefix);
    void setAllChecked(QListWidget *list, bool checked);
    void downloadCheckedItems(QListWidget *list);
    void downloadItems(const QVector<DownloadItem> &items);
    void ensureDownloadRow(const QString &url, const QString &title);
    void setDownloadRow(const QString &url, const QString &status,
                        int percent, const QString &detail);

protected:
    // 结果列表的鼠标事件：单击行勾选/取消（Shift+单击范围连选）；
    // 发现页底部状态行点击=加载下一页/失败重试。
    bool eventFilter(QObject *watched, QEvent *event) override;

private:

    // 下载目录处理：选目录（弹文件窗 + setDownloadsDirectory RPC）与
    // 不可写预警（Access denied 时只提示一次，避免刷屏）。
    void chooseDownloadsDirectory();
    void warnDownloadsDirectory(const QString &reason);
    bool m_dirWarned = false;

    // 文件名命名段：勾选状态 -> 模板串（{number}. {artists} - {name} ({mix_name})），
    // 模板串 -> 勾选回填，以及刷新"示例：xxx.flac"预览。
    QString buildNameTemplate() const;
    void applyNameTemplate(const QString &tmpl);
    void refreshNamePreview();
};
