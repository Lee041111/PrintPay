// =============================================================================
// 文件名：src/main.cpp
// 作用  ：程序入口。顺序很重要：
//           1) 单实例检查 —— 决不允许两个实例同时跑，否则同一个作业会被收两次钱；
//           2) 打开数据库并建表（首次运行会创建默认管理员 admin / admin123，并提示修改）；
//           3) 创建主窗口并显示。
// 注意  ：单实例用 QLockFile 实现（放在系统临时目录），程序异常退出后锁会自动释放，
//         不会出现“上次崩了这次就再也打不开”的情况。
// =============================================================================
#include <QApplication>
#include <QDir>
#include <QLockFile>
#include <QMessageBox>
#include <QStandardPaths>
#include <QTimer>

#include <cstdio>

#include "core/settings.h"
#include "core/store.h"
#include "ui/approval_dialog.h"
#include "ui/login_dialog.h"
#include "ui/main_window.h"
#include "ui/payment_dialog.h"

int main(int argc, char* argv[])
{
    QApplication application(argc, argv);
    QApplication::setApplicationName(QStringLiteral("PrintPay"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    // ---------- 1) 单实例检查 ----------
    const QString lockPath = QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
                                 .filePath(QStringLiteral("printpay_single_instance.lock"));
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);   // 0 表示不做“陈旧锁”判断，由系统在进程结束时自动释放
    if (!lock.tryLock(100)) {
        QMessageBox::warning(nullptr, QStringLiteral("打印计费助手"),
                             QStringLiteral("程序已经在运行了。\n\n"
                                            "请到任务栏找到已打开的窗口（同时运行两个会导致同一个作业被重复计费）。"));
        return 1;
    }

    // ---------- 2) 打开数据库 ----------
    printpay::Store store;
    const QString databasePath = QDir(QCoreApplication::applicationDirPath())
                                     .filePath(QStringLiteral("printpay.db"));
    QString error;
    if (!store.open(databasePath, &error)) {
        QMessageBox::critical(nullptr, QStringLiteral("打印计费助手"),
                              QStringLiteral("打开数据库失败：\n%1").arg(error));
        return 2;
    }
    QString createdPassword;
    if (!store.ensureSchema(&createdPassword, &error)) {
        QMessageBox::critical(nullptr, QStringLiteral("打印计费助手"),
                              QStringLiteral("初始化数据库失败：\n%1").arg(error));
        return 3;
    }

    // ---------- 3) 隐藏自检模式：把界面渲染成图片（开发期验证布局与收款码加载用） ----------
    // 用法：PrintPay.exe --selfcheck    会在程序目录的 selfcheck 子目录里生成 4 张 PNG
    // 注意：必须放在“首次运行提示”弹窗之前 —— 那个弹窗是模态的，离屏模式下没人能点它，
    //       会让自检永远卡住（这是开发过程中真实踩过的坑）。
    const QStringList arguments = QCoreApplication::arguments();
    const bool selfCheck = arguments.contains(QStringLiteral("--selfcheck"));
    // --minimized：启动时不显示窗口，直接进托盘（开机自启的快捷方式会带这个参数）
    const bool startMinimized = arguments.contains(QStringLiteral("--minimized")) ||
                                printpay::AppSettings::load().startMinimized;

    // ---------- 4) 首次运行提示（自检模式下跳过所有弹窗） ----------
    if (!createdPassword.isEmpty() && !selfCheck) {
        QMessageBox::information(
            nullptr, QStringLiteral("首次运行"),
            QStringLiteral("已创建默认管理员账号：\n\n"
                           "    用户名：admin\n"
                           "    密　码：%1\n\n"
                           "建议马上点主界面的“修改密码”改成自己的密码。\n"
                           "登录管理员后打印不收费；未登录或其他账号打印都会收费。")
                .arg(createdPassword));
    }

    if (selfCheck) {
        const QString outDir = QDir(QCoreApplication::applicationDirPath())
                                   .filePath(QStringLiteral("selfcheck"));
        QDir().mkpath(outDir);

        printpay::AppSettings settings = printpay::AppSettings::load();
        settings.printerName.clear();   // 自检不启动监控，避免干扰真实打印机

        printpay::PrintJobInfo job;
        job.jobId = 1;
        job.document = QStringLiteral("毕业论文最终版.pdf");
        job.userName = QStringLiteral("client");
        job.machineName = QStringLiteral("CLIENT-PC");
        job.totalPages = 7;
        job.printedPages = 7;
        job.copies = 1;
        job.printerName = QStringLiteral("Brother DCP-1618W");
        job.chargeable = true;

        printpay::MainWindow window(&store);
        window.resize(1100, 680);
        window.show();
        QApplication::processEvents();
        window.grab().save(outDir + QStringLiteral("/1_main.png"));

        printpay::ApprovalDialog approval(job, true, settings);
        approval.adjustSize();
        approval.grab().save(outDir + QStringLiteral("/2_approval.png"));

        printpay::PaymentDialog payment(job, 70, settings);
        payment.adjustSize();
        payment.grab().save(outDir + QStringLiteral("/3_payment.png"));

        printpay::LoginDialog login(&store);
        login.adjustSize();
        login.grab().save(outDir + QStringLiteral("/4_login.png"));

        std::printf("selfcheck 完成：%s\n", outDir.toUtf8().constData());
        return 0;
    }

    // ---------- 5) 托盘行为自测（开发期验证“点关闭按钮不会退出程序”） ----------
    if (arguments.contains(QStringLiteral("--traytest"))) {
        printpay::MainWindow window(&store);
        window.resize(900, 600);
        window.show();
        QApplication::processEvents();
        const bool visibleBefore = window.isVisible();
        // 500 毫秒后模拟“老板点了窗口右上角的关闭按钮”
        QTimer::singleShot(500, &window, [&window] { window.close(); });
        // 2 秒后检查：窗口应该被收进托盘（不可见），但程序必须还在运行
        QTimer::singleShot(2000, &application, [&window, visibleBefore] {
            std::printf("traytest: 关闭前窗口可见=%s，关闭后窗口可见=%s，程序仍在运行=%s\n",
                        visibleBefore ? "是" : "否",
                        window.isVisible() ? "是" : "否",
                        "是");
            std::fflush(stdout);
            QCoreApplication::quit();   // 直接退出事件循环，不再走 closeEvent
        });
        return application.exec();
    }

    // ---------- 6) 主窗口 ----------
    printpay::MainWindow window(&store);
    if (!startMinimized) {
        window.show();
    } else {
        // 不显示窗口时托盘图标就是唯一的入口，这里在日志里留个痕迹便于排查
        std::printf("已按配置在后台启动（窗口隐藏，可在任务栏托盘里找到）\n");
    }

    return application.exec();
}
