#pragma once

#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>

class QCheckBox;
class BackendClient;

// 登录对话框：用户在 UI 输入 Beatport 用户名/密码，提交后调后端 login RPC。
// 启动时若 authStatus 返回 ready=false，主窗口会弹出此对话框。
// 成功后凭据由后端写回配置+缓存，下次启动可直接跳过登录。
// "记住账号"勾选时用 QSettings 记住用户名，下次启动自动预填（密码不保存）。
class LoginDialog : public QDialog
{
    Q_OBJECT
public:
    explicit LoginDialog(BackendClient *client, QWidget *parent = nullptr);

signals:
    // 登录成功后发出，主窗口据此切换到正常界面。
    void loggedIn();

private slots:
    void onSubmit();
    void onLoginResult(bool ok, QString err);

private:
    BackendClient *m_client;
    QLineEdit *m_userEdit;
    QLineEdit *m_passEdit;
    QCheckBox *m_remember;
    QPushButton *m_loginBtn;
    QPushButton *m_cancelBtn;
    QLabel *m_statusLabel;
};
