// =============================================================================
// 文件名：src/ui/main_window.h
// 作用  ：主窗口。展示当前打印机状态、当前身份、今日营收、作业列表与运行日志，
//         并负责弹出审批窗与收款窗（业务逻辑都在 JobFlow 里）。
// 位置  ：程序的主界面，持有 JobFlow 与 Store。
// 注意  ：界面只做“显示 + 弹窗 + 转交决策”，不自己判断该不该收费 —— 判断逻辑
//         集中在 JobFlow，避免同一套规则在两处实现导致不一致（这是最常见的 bug 来源）。
// =============================================================================
#pragma once

#include <QMainWindow>
#include <QSystemTrayIcon>   // 槽函数签名用到它的嵌套枚举，必须包含完整定义（前向声明不够）

#include "core/job_flow.h"
#include "core/settings.h"
#include "core/store.h"

class QAction;
class QLabel;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;
class QTimer;

namespace printpay {

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    // 参数 store：已打开的数据库（由 main.cpp 创建，生命周期长于本窗口）
    explicit MainWindow(Store* store, QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

private slots:
    // ---- JobFlow 信号 ----
    void onApprovalRequested(const PrintJobInfo& job, bool willCharge);
    void onPaymentRequested(const PrintJobInfo& job, int amountCents);
    void onLogMessage(const QString& text);
    void onJobsChanged();
    void onPrinterPauseChanged(bool paused);

    // ---- 界面按钮 ----
    void onLoginClicked();
    void onLogoutClicked();
    void onSettingsClicked();
    void onStartStopClicked();
    void onResumePrinterClicked();
    void onSimulateClicked();
    void onChangePasswordClicked();
    // 以管理员身份重启自己（暂停打印机、取消作业都需要管理员权限）
    void onRelaunchElevatedClicked();

    // ---- 托盘（常驻任务栏）----
    void onTrayActivated(QSystemTrayIcon::ActivationReason reason);
    void showMainWindow();
    void hideToTray();
    void quitApplication();

private:
    void buildUi();
    void buildTray();                       // 创建托盘图标与右键菜单
    void applySettingsToFlow(bool restartMonitoring);
    void refreshHeader();
    void refreshSummary();
    void refreshJobsTable();
    void appendLog(const QString& text);
    void writeLogToFile(const QString& text);
    void loadWindowIcon();                  // 载入 assets/app.ico（缺图时画一个简单的）
    void notifyUser(const QString& title, const QString& text);   // 托盘气泡提醒
    void makeSureVisible();                 // 有弹窗要显示时，确保主窗口可见并置前
    bool trayAvailable_ = false;

    Store* store_ = nullptr;
    JobFlow* flow_ = nullptr;
    AppSettings settings_;

    QLabel* printerLabel_ = nullptr;
    QLabel* pauseLabel_ = nullptr;
    QLabel* accountLabel_ = nullptr;
    QLabel* summaryLabel_ = nullptr;
    QLabel* privilegeLabel_ = nullptr;      // 权限提示条（没有管理员权限时显示）
    QPushButton* elevateButton_ = nullptr;  // “以管理员身份重启”按钮
    QTableWidget* jobsTable_ = nullptr;
    QPlainTextEdit* logView_ = nullptr;
    QPushButton* startStopButton_ = nullptr;
    QPushButton* loginButton_ = nullptr;
    QPushButton* logoutButton_ = nullptr;
    QTimer* summaryTimer_ = nullptr;

    // ---- 托盘（常驻任务栏）----
    QSystemTrayIcon* trayIcon_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* trayShowAction_ = nullptr;
    QAction* trayMonitorAction_ = nullptr;
    QAction* trayResumeAction_ = nullptr;
    QAction* trayQuitAction_ = nullptr;
    bool forceQuit_ = false;        // 由“退出程序”置位，表示这次关闭是真的要退出
};

}  // namespace printpay
