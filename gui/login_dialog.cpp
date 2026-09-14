#include "login_dialog.h"
#include "backend_client.h"

#include <QFormLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QCheckBox>
#include <QSettings>
#include <QIcon>

namespace {
const char *kSettingsOrg = "BeatportDL";
const char *kSettingsApp = "BeatportDownloader";
const char *kKeyUsername = "username";
}

LoginDialog::LoginDialog(BackendClient *client, QWidget *parent)
    : QDialog(parent), m_client(client)
{
    setWindowTitle(tr("Beatport 登录"));
    setWindowFlag(Qt::Dialog);
    setMinimumWidth(380);

    auto *form = new QFormLayout;
    m_userEdit = new QLineEdit(this);
    m_userEdit->setPlaceholderText(tr("Beatport 用户名"));
    m_passEdit = new QLineEdit(this);
    m_passEdit->setPlaceholderText(tr("Beatport 密码"));
    m_passEdit->setEchoMode(QLineEdit::Password);
    form->addRow(tr("用户名："), m_userEdit);
    form->addRow(tr("密码："), m_passEdit);

    // 记住账号：仅记住用户名（QSettings 本地存储），密码永不保存。
    m_remember = new QCheckBox(tr("记住账号"), this);
    m_remember->setChecked(true);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setStyleSheet("color: #c0392b;");

    auto *btnRow = new QHBoxLayout;
    btnRow->addStretch();
    m_loginBtn = new QPushButton(tr("登录"), this);
    m_loginBtn->setDefault(true);
    m_cancelBtn = new QPushButton(tr("取消"), this);
    btnRow->addWidget(m_loginBtn);
    btnRow->addWidget(m_cancelBtn);

    auto *root = new QVBoxLayout(this);
    root->addLayout(form);
    root->addWidget(m_remember);
    root->addWidget(m_statusLabel);
    root->addLayout(btnRow);

    // 记住的用户名预填（不预填密码）。
    QSettings settings(kSettingsOrg, kSettingsApp);
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
        m_statusLabel->setText(tr("用户名和密码不能为空"));
        return;
    }
    // 登录是网络操作，禁用按钮防重复点击
    m_loginBtn->setEnabled(false);
    m_cancelBtn->setEnabled(false);
    m_statusLabel->setText(tr("登录中…"));
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
            QSettings settings(kSettingsOrg, kSettingsApp);
            settings.setValue(QString::fromLatin1(kKeyUsername), m_userEdit->text().trimmed());
        } else {
            QSettings settings(kSettingsOrg, kSettingsApp);
            settings.remove(QString::fromLatin1(kKeyUsername));
        }
        m_statusLabel->clear();
        emit loggedIn();
        accept();
    } else {
        m_statusLabel->setText(tr("登录失败：%1").arg(err));
    }
}
