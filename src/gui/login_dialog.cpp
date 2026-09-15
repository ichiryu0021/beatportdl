#include "login_dialog.h"
#include "backend_client.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QSettings>
#include <QIcon>
#include <QCoreApplication>
#include <QDir>

namespace {
const char *kKeyUsername = "username";

// 便携化：记住的用户名保存在 exe 旁的 ini 中，不写入系统注册表/用户目录。
QString portableSettingsPath()
{
    return QCoreApplication::applicationDirPath()
           + QDir::separator() + QLatin1String("bpdownloader-gui.ini");
}
}

LoginDialog::LoginDialog(BackendClient *client, QWidget *parent)
    : QDialog(parent), m_client(client)
{
    setWindowTitle(tr("BPDownloader Sign In"));
    setWindowFlag(Qt::Dialog);
    setMinimumWidth(380);

    auto *form = new QFormLayout;
    m_userEdit = new QLineEdit(this);
    m_userEdit->setPlaceholderText(tr("Beatport username"));
    m_passEdit = new QLineEdit(this);
    m_passEdit->setPlaceholderText(tr("Beatport password"));
    m_passEdit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("Username:"), m_userEdit);
    form->addRow(tr("Password:"), m_passEdit);

    // 记住账号：仅记住用户名（QSettings 本地存储），密码永不保存。
    m_remember = new QCheckBox(tr("Remember me"), this);
    m_remember->setChecked(true);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color: #c0392b;");

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    m_loginBtn = new QPushButton(tr("Sign In"), this);
    m_loginBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("Cancel"), this);
    btnRow->addWidget(m_loginBtn);
    btnRow->addWidget(m_cancelBtn);

    auto *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_remember);
    root->addWidget(m_statusLabel);
    root->addLayout(btnRow);

    // 记住的用户名预填（不预填密码）。
    QSettings settings(portableSettingsPath(), QSettings::IniFormat);
    const QString savedUser = settings.value(QString::fromLatin1(kKeyUsername)).toString();
    if (!savedUser.isEmpty())
        m_userEdit->setText(savedUser);

    connect(m_loginBtn, &QPushButton::clicked, this, &LoginDialog::onSubmit);
    connect(m_cancelBtn, &QPushButton::clicked, this, &QDialog::reject);
    // Enter 键在密码框也会触发提交
    connect(m_passEdit, &QLineEdit::returnPressed, this, &LoginDialog::onSubmit);
}

void LoginDialog::onSubmit()
{
    const QString user = m_userEdit->text().trimmed();
    const QString pass = m_passEdit->text();
    if (user.isEmpty() || pass.isEmpty()) {
        m_statusLabel->setText(tr("Username and password must not be empty."));
        return;
    }
    // 登录是网络操作，禁用按钮防重复点击
    m_loginBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);
    m_statusLabel->setText(tr("Signing in..."));
    m_client->login(user, pass,
        [this]() { onLoginResult(true, QString()); },
        [this](QString err) { onLoginResult(false, err); });
}

void LoginDialog::onLoginResult(bool ok, QString err)
{
    m_loginBtn->setEnabled(true);
    m_cancelBtn->setEnabled(true);
    if (ok) {
        if (m_remember->isChecked()) {
            QSettings settings(portableSettingsPath(), QSettings::IniFormat);
            settings.setValue(QString::fromLatin1(kKeyUsername), m_userEdit->text().trimmed());
        } else {
            QSettings settings(portableSettingsPath(), QSettings::IniFormat);
            settings.remove(QString::fromLatin1(kKeyUsername));
        }
        m_statusLabel->clear();
        emit loggedIn();
        accept();
    } else {
        // 后端错误为英文技术文案，常见项翻译后展示。
        QString zh = err;
        if (err.contains(QStringLiteral("Incorrect username or password"),
                         Qt::CaseInsensitive))
            zh = tr("Incorrect username or password. Please check and try again.");
        else if (err.contains(QStringLiteral("Network"))
                 || err.contains(QStringLiteral("Connection"))
                 || err.contains(QStringLiteral("timed out"), Qt::CaseInsensitive))
            zh = tr("Network connection failed. Please check your network or proxy settings.");
        m_statusLabel->setText(tr("Sign in failed: %1").arg(zh));
    }
}
