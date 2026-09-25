// =============================================================================
// 文件名：src/ui/login_dialog.h
// 作用  ：登录窗口。支持三种入口：
//           ① 管理员登录（免费、无需审批）；
//           ② 普通账号登录（按配置收费、可选审批）；
//           ③ 直接取消 —— 以“未登录”状态使用（所有打印作业都收费）。
// 位置  ：由 MainWindow 的“登录 / 切换账号”按钮打开。
// 注意  ：登录只是“本程序内部的身份标识”，不影响 Windows 用户的打印权限；
//         口令以 SHA-256 摘要存库（Store::hashPassword），库中不保存明文。
// =============================================================================
#pragma once

#include <QDialog>

#include "core/job_flow.h"
#include "core/store.h"

class QLabel;
class QLineEdit;

namespace printpay {

class LoginDialog : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(Store* store, QWidget* parent = nullptr);

    // 登录成功后的会话（取消时对应“未登录”）
    Session session() const { return session_; }

private slots:
    void onLogin();
    void onCreateAccount();

private:
    Store* store_ = nullptr;
    QLineEdit* userEdit_ = nullptr;
    QLineEdit* passwordEdit_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    Session session_;
};

}  // namespace printpay
