// =============================================================================
// 文件名：src/ui/login_dialog.cpp
// 作用  ：登录窗口实现。登录成功后 session_ 带用户名与角色；
//         点“取消”则表示以未登录状态继续（所有作业按“其他人”策略处理：收费）。
// =============================================================================
#include "ui/login_dialog.h"

#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace printpay {

LoginDialog::LoginDialog(Store* store, QWidget* parent)
    : QDialog(parent)
    , store_(store)
{
    setWindowTitle(QStringLiteral("登录 · 打印计费助手"));
    setMinimumWidth(420);

    QVBoxLayout* root = new QVBoxLayout(this);

    QLabel* title = new QLabel(QStringLiteral("请选择身份"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(16);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    QLabel* explain = new QLabel(
        QStringLiteral("管理员登录：打印免费且不打扰\n"
                       "普通账号 / 未登录：打印收费，按配置可能需要在打印前征得同意"),
        this);
    explain->setAlignment(Qt::AlignCenter);
    explain->setStyleSheet(QStringLiteral("color:#555555;"));
    root->addWidget(explain);

    QFormLayout* form = new QFormLayout;
    userEdit_ = new QLineEdit(this);
    userEdit_->setPlaceholderText(QStringLiteral("用户名（管理员默认 admin）"));
    passwordEdit_ = new QLineEdit(this);
    passwordEdit_->setEchoMode(QLineEdit::Password);
    passwordEdit_->setPlaceholderText(QStringLiteral("密码（首次默认 admin123，请尽快修改）"));
    form->addRow(QStringLiteral("用户名："), userEdit_);
    form->addRow(QStringLiteral("密码："), passwordEdit_);
    root->addLayout(form);

    hintLabel_ = new QLabel(this);
    hintLabel_->setStyleSheet(QStringLiteral("color:#c81e1e;"));
    hintLabel_->setWordWrap(true);
    root->addWidget(hintLabel_);

    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* createButton = new QPushButton(QStringLiteral("创建普通账号"), this);
    QPushButton* cancelButton = new QPushButton(QStringLiteral("取消（以未登录状态使用）"), this);
    QPushButton* loginButton = new QPushButton(QStringLiteral("登录"), this);
    loginButton->setDefault(true);
    buttons->addWidget(createButton);
    buttons->addStretch(1);
    buttons->addWidget(cancelButton);
    buttons->addWidget(loginButton);
    root->addLayout(buttons);

    connect(loginButton, &QPushButton::clicked, this, &LoginDialog::onLogin);
    connect(createButton, &QPushButton::clicked, this, &LoginDialog::onCreateAccount);
    connect(cancelButton, &QPushButton::clicked, this, [this] {
        // “取消”在这里的含义是：以未登录状态继续使用（所有作业按“其他人”策略收费）。
        // 用 accept() 而不是 reject()，是为了和“直接点右上角关闭窗口”区分开 ——
        // 后者走 reject()，表示什么都不改（不改变当前登录状态）。
        session_ = Session();
        accept();
    });
    connect(passwordEdit_, &QLineEdit::returnPressed, this, &LoginDialog::onLogin);
}

void LoginDialog::onLogin()
{
    if (store_ == nullptr || !store_->isOpen()) {
        hintLabel_->setText(QStringLiteral("数据库不可用，无法登录"));
        return;
    }
    const QString user = userEdit_->text().trimmed();
    const QString password = passwordEdit_->text();
    if (user.isEmpty() || password.isEmpty()) {
        hintLabel_->setText(QStringLiteral("请填写用户名和密码"));
        return;
    }

    AccountRole role = AccountRole::User;
    QString error;
    if (!store_->authenticate(user, password, &role, &error)) {
        hintLabel_->setText(error.isEmpty() ? QStringLiteral("登录失败") : error);
        return;
    }
    session_.loggedIn = true;
    session_.userName = user;
    session_.role = role;
    accept();
}

void LoginDialog::onCreateAccount()
{
    if (store_ == nullptr || !store_->isOpen()) {
        hintLabel_->setText(QStringLiteral("数据库不可用，无法创建账号"));
        return;
    }
    const QString user = userEdit_->text().trimmed();
    const QString password = passwordEdit_->text();
    if (user.isEmpty() || password.size() < 6) {
        hintLabel_->setText(QStringLiteral("创建普通账号：用户名不能为空，密码至少 6 位"));
        return;
    }
    QString error;
    if (!store_->addAccount(user, password, AccountRole::User, &error)) {
        hintLabel_->setText(error);
        return;
    }
    hintLabel_->setStyleSheet(QStringLiteral("color:#117a37;"));
    hintLabel_->setText(QStringLiteral("普通账号「%1」已创建，现在可以直接登录").arg(user));
}

}  // namespace printpay
