// =============================================================================
// 文件名：src/ui/main_window.cpp
// 作用  ：主窗口实现（界面搭建、托盘常驻、信号连接、弹窗调度、日志与汇总刷新）。
// 常驻设计：程序会一直待在任务栏托盘里 —— 点关闭按钮只是把窗口收起来，
//           监控、审批、收款都不中断；要真正退出请用托盘菜单的“退出程序”。
// =============================================================================
#include "ui/main_window.h"

#include <QAbstractItemView>
#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPixmap>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX   // 禁止 windows.h 定义 min/max 宏，避免与 Qt / 标准库冲突
#endif
#include <windows.h>
#include <shellapi.h>   // ShellExecuteW：用于“以管理员身份重启”
#endif

#include "core/billing.h"
#include "ui/approval_dialog.h"
#include "ui/login_dialog.h"
#include "ui/payment_dialog.h"
#include "ui/settings_dialog.h"

namespace printpay {
namespace {

// 日志文件最大 4MB：超过就滚动成 printpay.log.old，避免长期运行把磁盘写满
constexpr qint64 kMaxLogBytes = 4 * 1024 * 1024;

QString logFilePath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("printpay.log"));
}

QTableWidgetItem* makeItem(const QString& text, Qt::Alignment alignment = Qt::AlignLeft | Qt::AlignVCenter)
{
    QTableWidgetItem* item = new QTableWidgetItem(text);
    item->setTextAlignment(alignment);
    item->setFlags(item->flags() & ~Qt::ItemIsEditable);
    return item;
}

// 作业状态的中文显示
QString stateText(JobState state)
{
    switch (state) {
    case JobState::Monitoring:     return QStringLiteral("排队中");
    case JobState::WaitingApprove: return QStringLiteral("等待同意");
    case JobState::Printing:       return QStringLiteral("打印中");
    case JobState::Finished:       return QStringLiteral("已结算");
    case JobState::Canceled:       return QStringLiteral("已取消");
    }
    return QStringLiteral("未知");
}

}  // namespace

MainWindow::MainWindow(Store* store, QWidget* parent)
    : QMainWindow(parent)
    , store_(store)
{
    buildUi();
    loadWindowIcon();
    buildTray();

    flow_ = new JobFlow(store_, this);
    connect(flow_, &JobFlow::approvalRequested, this, &MainWindow::onApprovalRequested);
    connect(flow_, &JobFlow::paymentRequested, this, &MainWindow::onPaymentRequested);
    connect(flow_, &JobFlow::logMessage, this, &MainWindow::onLogMessage);
    connect(flow_, &JobFlow::jobsChanged, this, &MainWindow::onJobsChanged);
    connect(flow_, &JobFlow::printerPauseChanged, this, &MainWindow::onPrinterPauseChanged);

    settings_ = AppSettings::load();
    flow_->setSettings(settings_);
    flow_->setSession(Session());   // 启动时默认未登录：所有作业都按“其他人”策略收费

    // 每 10 秒刷一次今日汇总（作业少，没必要更频繁；避免频繁查库）
    summaryTimer_ = new QTimer(this);
    summaryTimer_->setInterval(10000);
    connect(summaryTimer_, &QTimer::timeout, this, &MainWindow::refreshSummary);
    summaryTimer_->start();

    applySettingsToFlow(false);
    refreshHeader();
    refreshSummary();
    refreshJobsTable();
    appendLog(QStringLiteral("程序已启动。数据库：%1")
                  .arg(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("printpay.db"))));
    appendLog(QStringLiteral("任务栏托盘：%1（点关闭按钮只会收起窗口，程序继续在后台监控）")
                  .arg(trayAvailable_ ? QStringLiteral("可用") : QStringLiteral("不可用")));
}

MainWindow::~MainWindow()
{
    if (flow_ != nullptr) {
        flow_->stop();   // 退出前恢复被本程序暂停的打印机
    }
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("打印计费助手 PrintPay"));
    resize(1100, 680);

    QWidget* central = new QWidget(this);
    QVBoxLayout* root = new QVBoxLayout(central);

    // ---------------- 顶部状态与控制 ----------------
    QGroupBox* topBox = new QGroupBox(QStringLiteral("运行状态"), central);
    QVBoxLayout* topLayout = new QVBoxLayout(topBox);

    QHBoxLayout* statusRow = new QHBoxLayout;
    printerLabel_ = new QLabel(topBox);
    pauseLabel_ = new QLabel(topBox);
    accountLabel_ = new QLabel(topBox);
    statusRow->addWidget(printerLabel_, 1);
    statusRow->addWidget(pauseLabel_);
    statusRow->addWidget(accountLabel_);
    topLayout->addLayout(statusRow);

    summaryLabel_ = new QLabel(topBox);
    summaryLabel_->setStyleSheet(QStringLiteral("font-size:15px;color:#1a4f8a;"));
    topLayout->addWidget(summaryLabel_);

    // 权限提示条：没有管理员权限时，“打印前先拦住”这个功能是失效的，必须显著提示
    QHBoxLayout* privilegeRow = new QHBoxLayout;
    privilegeLabel_ = new QLabel(topBox);
    privilegeLabel_->setWordWrap(true);
    privilegeLabel_->setStyleSheet(
        QStringLiteral("background:#fff4e5;color:#8a4b00;padding:6px;border:1px solid #ffd591;"));
    privilegeLabel_->setText(QStringLiteral(
        "⚠ 当前不是以管理员身份运行：可以统计页数、收费与弹出收款码，"
        "但无法暂停打印机队列，也就是“客户打印前需要你同意”这一条会失效。"));
    elevateButton_ = new QPushButton(QStringLiteral("以管理员身份重启"), topBox);
    privilegeRow->addWidget(privilegeLabel_, 1);
    privilegeRow->addWidget(elevateButton_);
    topLayout->addLayout(privilegeRow);
    privilegeLabel_->hide();
    elevateButton_->hide();

    QHBoxLayout* buttonRow = new QHBoxLayout;
    startStopButton_ = new QPushButton(QStringLiteral("开始监控"), topBox);
    QPushButton* resumeButton = new QPushButton(QStringLiteral("恢复打印机"), topBox);
    QPushButton* simulateButton = new QPushButton(QStringLiteral("模拟一次打印（测试用）"), topBox);
    QPushButton* settingsButton = new QPushButton(QStringLiteral("设置"), topBox);
    QPushButton* passwordButton = new QPushButton(QStringLiteral("修改密码"), topBox);
    loginButton_ = new QPushButton(QStringLiteral("登录"), topBox);
    logoutButton_ = new QPushButton(QStringLiteral("退出登录"), topBox);
    logoutButton_->setEnabled(false);

    buttonRow->addWidget(startStopButton_);
    buttonRow->addWidget(resumeButton);
    buttonRow->addWidget(simulateButton);
    buttonRow->addStretch(1);
    buttonRow->addWidget(settingsButton);
    buttonRow->addWidget(passwordButton);
    buttonRow->addWidget(loginButton_);
    buttonRow->addWidget(logoutButton_);
    topLayout->addLayout(buttonRow);
    root->addWidget(topBox);

    // ---------------- 中部：作业列表 + 日志 ----------------
    QSplitter* splitter = new QSplitter(Qt::Horizontal, central);

    jobsTable_ = new QTableWidget(0, 6, splitter);
    jobsTable_->setHorizontalHeaderLabels({
        QStringLiteral("作业号"), QStringLiteral("文档"), QStringLiteral("页数"),
        QStringLiteral("份数"), QStringLiteral("状态"), QStringLiteral("是否收费")
    });
    jobsTable_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    jobsTable_->horizontalHeader()->setStretchLastSection(true);
    jobsTable_->setColumnWidth(0, 80);
    jobsTable_->setColumnWidth(1, 300);
    jobsTable_->setColumnWidth(2, 70);
    jobsTable_->setColumnWidth(3, 60);
    jobsTable_->setColumnWidth(4, 100);
    jobsTable_->verticalHeader()->setVisible(false);
    jobsTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    jobsTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    splitter->addWidget(jobsTable_);

    logView_ = new QPlainTextEdit(splitter);
    logView_->setReadOnly(true);
    logView_->setMaximumBlockCount(2000);   // 只保留最近 2000 行，防止内存无限增长
    splitter->addWidget(logView_);
    splitter->setStretchFactor(0, 3);
    splitter->setStretchFactor(1, 2);
    root->addWidget(splitter, 1);

    setCentralWidget(central);

    // ---------------- 状态栏 ----------------
    const QString appDir = QCoreApplication::applicationDirPath();
    statusBar()->showMessage(QStringLiteral("数据目录：%1").arg(appDir));

    // ---------------- 信号连接 ----------------
    connect(startStopButton_, &QPushButton::clicked, this, &MainWindow::onStartStopClicked);
    connect(resumeButton, &QPushButton::clicked, this, &MainWindow::onResumePrinterClicked);
    connect(simulateButton, &QPushButton::clicked, this, &MainWindow::onSimulateClicked);
    connect(settingsButton, &QPushButton::clicked, this, &MainWindow::onSettingsClicked);
    connect(passwordButton, &QPushButton::clicked, this, &MainWindow::onChangePasswordClicked);
    connect(loginButton_, &QPushButton::clicked, this, &MainWindow::onLoginClicked);
    connect(logoutButton_, &QPushButton::clicked, this, &MainWindow::onLogoutClicked);
    connect(elevateButton_, &QPushButton::clicked, this, &MainWindow::onRelaunchElevatedClicked);
}

void MainWindow::loadWindowIcon()
{
    // 优先用 assets/app.ico；文件缺失时用系统图标兜底，保证托盘上一定有东西显示
    const QString iconPath = AppSettings::assetPath(QStringLiteral("app.ico"));
    QIcon icon;
    if (QFileInfo::exists(iconPath)) {
        icon = QIcon(iconPath);
    }
    if (icon.isNull()) {
        icon = style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    setWindowIcon(icon);
    qApp->setWindowIcon(icon);
}

void MainWindow::buildTray()
{
    trayAvailable_ = QSystemTrayIcon::isSystemTrayAvailable();
    if (!trayAvailable_) {
        return;
    }

    trayMenu_ = new QMenu(this);
    trayShowAction_ = trayMenu_->addAction(QStringLiteral("显示主窗口"));
    trayMonitorAction_ = trayMenu_->addAction(QStringLiteral("开始监控"));
    trayResumeAction_ = trayMenu_->addAction(QStringLiteral("恢复打印机"));
    trayMenu_->addSeparator();
    trayQuitAction_ = trayMenu_->addAction(QStringLiteral("退出程序"));

    connect(trayShowAction_, &QAction::triggered, this, &MainWindow::showMainWindow);
    connect(trayMonitorAction_, &QAction::triggered, this, &MainWindow::onStartStopClicked);
    connect(trayResumeAction_, &QAction::triggered, this, &MainWindow::onResumePrinterClicked);
    connect(trayQuitAction_, &QAction::triggered, this, &MainWindow::quitApplication);

    trayIcon_ = new QSystemTrayIcon(windowIcon(), this);
    trayIcon_->setContextMenu(trayMenu_);
    trayIcon_->setToolTip(QStringLiteral("打印计费助手：正在运行"));
    connect(trayIcon_, &QSystemTrayIcon::activated, this, &MainWindow::onTrayActivated);
    trayIcon_->show();
}

void MainWindow::onTrayActivated(QSystemTrayIcon::ActivationReason reason)
{
    // 双击托盘图标：在“显示窗口 / 收起窗口”之间切换（单击不处理，避免误触）
    if (reason == QSystemTrayIcon::DoubleClick) {
        if (isVisible() && !isMinimized()) {
            hideToTray();
        } else {
            showMainWindow();
        }
    }
}

void MainWindow::showMainWindow()
{
    showNormal();       // 从最小化/隐藏状态恢复
    raise();
    activateWindow();
}

void MainWindow::hideToTray()
{
    hide();
    if (trayAvailable_) {
        notifyUser(QStringLiteral("打印计费助手"),
                   QStringLiteral("已最小化到托盘，程序继续监控打印。双击托盘图标可重新打开窗口。"));
    }
}

void MainWindow::quitApplication()
{
    forceQuit_ = true;   // 告诉 closeEvent：这次是真的退出
    close();
}

void MainWindow::notifyUser(const QString& title, const QString& text)
{
    if (trayAvailable_ && trayIcon_ != nullptr) {
        trayIcon_->showMessage(title, text, QSystemTrayIcon::Information, 6000);
    }
}

void MainWindow::makeSureVisible()
{
    // 有弹窗要显示时，把主窗口恢复出来并置前，避免窗口被收在托盘里、老板看不到提示
    if (!isVisible() || isMinimized()) {
        showMainWindow();
    }
    raise();
    activateWindow();
}

void MainWindow::applySettingsToFlow(bool restartMonitoring)
{
    flow_->setSettings(settings_);

    const bool shouldRun = !settings_.printerName.isEmpty() &&
                           (restartMonitoring || !flow_->isRunning() || flow_->printerName() != settings_.printerName);

    if (shouldRun) {
        if (flow_->isRunning()) {
            flow_->stop();
        }
        QString error;
        if (!flow_->start(settings_.printerName, &error)) {
            appendLog(QStringLiteral("启动监控失败：%1").arg(error));
            QMessageBox::warning(this, QStringLiteral("打印计费助手"), error);
        }
    }
    refreshHeader();
}

void MainWindow::onApprovalRequested(const PrintJobInfo& job, bool willCharge)
{
    makeSureVisible();   // 客户在等，先把窗口亮出来
    notifyUser(QStringLiteral("有客户要打印"),
               QStringLiteral("文档：%1，%2 页。请确认是否同意打印。")
                   .arg(job.document.isEmpty() ? QStringLiteral("(未知)") : job.document)
                   .arg(job.billablePages() > 0 ? QString::number(job.billablePages())
                                                : QStringLiteral("页数未知")));

    // 模态弹窗：老板必须明确选择，弹窗期间 JobFlow 的轮询逻辑被屏蔽（见 job_flow.cpp）
    ApprovalDialog dialog(job, willCharge, settings_, this);
    dialog.exec();

    switch (dialog.decision()) {
    case ApprovalDialog::Decision::Approve:
        flow_->approveJob(job.jobId, false);
        break;
    case ApprovalDialog::Decision::ApproveFree:
        flow_->approveJob(job.jobId, true);
        break;
    case ApprovalDialog::Decision::Reject:
        flow_->rejectJob(job.jobId);
        break;
    }
    refreshJobsTable();
    refreshHeader();
}

void MainWindow::onPaymentRequested(const PrintJobInfo& job, int amountCents)
{
    makeSureVisible();
    notifyUser(QStringLiteral("打印完成，请收款"),
               QStringLiteral("%1，应收 %2")
                   .arg(job.document.isEmpty() ? QStringLiteral("(未知文档)") : job.document)
                   .arg(formatCents(amountCents)));

    PaymentDialog dialog(job, amountCents, settings_, this);
    dialog.exec();
    // 无论“已收款”还是“未收款”，都要把流水记下来：未收款会体现在今日未收金额里
    flow_->finishPayment(job.jobId, dialog.pages(), dialog.paid());
    refreshJobsTable();
    refreshSummary();
}

void MainWindow::onLogMessage(const QString& text)
{
    appendLog(text);
}

void MainWindow::onJobsChanged()
{
    refreshJobsTable();
    refreshHeader();
}

void MainWindow::onPrinterPauseChanged(bool paused)
{
    Q_UNUSED(paused)
    refreshHeader();
}

void MainWindow::onLoginClicked()
{
    LoginDialog dialog(store_, this);
    if (dialog.exec() == QDialog::Accepted) {
        flow_->setSession(dialog.session());
        refreshHeader();
        if (dialog.session().loggedIn) {
            appendLog(QStringLiteral("已登录：%1").arg(dialog.session().displayName()));
        } else {
            appendLog(QStringLiteral("未登录状态：所有打印作业按“其他人”策略处理（收费）"));
        }
    }
}

void MainWindow::onLogoutClicked()
{
    flow_->setSession(Session());
    refreshHeader();
    appendLog(QStringLiteral("已退出登录：所有打印作业按“其他人”策略处理（收费）"));
}

void MainWindow::onSettingsClicked()
{
    SettingsDialog dialog(settings_, this);
    if (dialog.exec() != QDialog::Accepted) {
        return;
    }
    settings_ = dialog.result();
    QString error;
    if (!settings_.save(&error)) {
        QMessageBox::warning(this, QStringLiteral("打印计费助手"), error);
    }
    appendLog(QStringLiteral("设置已保存：打印机「%1」，单价 %2/页，微信号 %3")
                  .arg(settings_.printerName)
                  .arg(formatCents(settings_.pricePerPageCents))
                  .arg(settings_.wechatId));
    applySettingsToFlow(true);   // 打印机可能变了，直接按新配置重启监控
}

void MainWindow::onStartStopClicked()
{
    if (flow_->isRunning()) {
        flow_->stop();
        appendLog(QStringLiteral("已停止监控"));
    } else {
        applySettingsToFlow(true);
    }
    refreshHeader();
}

void MainWindow::onResumePrinterClicked()
{
    QString error;
    if (flow_->forceResumePrinter(&error)) {
        appendLog(QStringLiteral("已恢复打印机工作"));
    } else {
        QMessageBox::warning(this, QStringLiteral("打印计费助手"),
                             error.isEmpty() ? QStringLiteral("恢复打印机失败") : error);
    }
    refreshHeader();
}

void MainWindow::onSimulateClicked()
{
    bool ok = false;
    const int pages = QInputDialog::getInt(this, QStringLiteral("模拟打印作业"),
                                           QStringLiteral("模拟打印多少页？（用于测试收费流程，不会真的打印）"),
                                           10, 1, 100000, 1, &ok);
    if (!ok) {
        return;
    }
    if (!flow_->isRunning()) {
        QMessageBox::information(this, QStringLiteral("打印计费助手"),
                                 QStringLiteral("请先点“开始监控”并选好打印机"));
        return;
    }
    flow_->simulateJob(pages);
}

void MainWindow::onChangePasswordClicked()
{
    const Session session = flow_->session();
    if (!session.loggedIn) {
        QMessageBox::information(this, QStringLiteral("打印计费助手"),
                                 QStringLiteral("请先登录，再修改密码"));
        return;
    }
    bool ok = false;
    const QString newPassword = QInputDialog::getText(
        this, QStringLiteral("修改密码"),
        QStringLiteral("请输入账号「%1」的新密码（至少 6 位）：").arg(session.userName),
        QLineEdit::Password, QString(), &ok);
    if (!ok) {
        return;
    }
    QString error;
    if (store_->changePassword(session.userName, newPassword, &error)) {
        QMessageBox::information(this, QStringLiteral("打印计费助手"),
                                 QStringLiteral("密码已修改，请牢记新密码"));
        appendLog(QStringLiteral("账号「%1」的密码已修改").arg(session.userName));
    } else {
        QMessageBox::warning(this, QStringLiteral("打印计费助手"), error);
    }
}

void MainWindow::refreshHeader()
{
    // 打印机状态
    if (flow_->isRunning()) {
        QString error;
        const bool paused = flow_->printerPaused(&error);
        printerLabel_->setText(QStringLiteral("监控中：%1").arg(flow_->printerName()));
        pauseLabel_->setText(paused ? QStringLiteral("打印机：已暂停（等待同意）")
                                    : QStringLiteral("打印机：工作中"));
        pauseLabel_->setStyleSheet(paused ? QStringLiteral("color:#c81e1e;font-weight:bold;")
                                          : QStringLiteral("color:#117a37;"));
        startStopButton_->setText(QStringLiteral("停止监控"));
    } else {
        printerLabel_->setText(QStringLiteral("未监控（请在“设置”里选择打印机并开始监控）"));
        pauseLabel_->setText(QStringLiteral("打印机：未监控"));
        pauseLabel_->setStyleSheet(QStringLiteral("color:#666666;"));
        startStopButton_->setText(QStringLiteral("开始监控"));
    }

    // 当前身份
    const Session session = flow_->session();
    accountLabel_->setText(QStringLiteral("当前身份：%1").arg(session.displayName()));
    loginButton_->setEnabled(!session.loggedIn);
    logoutButton_->setEnabled(session.loggedIn);

    // 权限提示：只有在监控中且确实没有控制权限时才提示，避免无意义地打扰老板
    const bool lackPrivilege = flow_->isRunning() && !flow_->canControlPrinter();
    privilegeLabel_->setVisible(lackPrivilege);
    elevateButton_->setVisible(lackPrivilege);

    // 托盘菜单与提示文字跟着状态走
    if (trayIcon_ != nullptr) {
        trayIcon_->setToolTip(QStringLiteral("打印计费助手\n%1\n%2")
                                  .arg(flow_->isRunning()
                                           ? QStringLiteral("监控中：%1").arg(flow_->printerName())
                                           : QStringLiteral("未监控"),
                                       session.displayName()));
    }
    if (trayMonitorAction_ != nullptr) {
        trayMonitorAction_->setText(flow_->isRunning() ? QStringLiteral("停止监控")
                                                       : QStringLiteral("开始监控"));
    }
}

void MainWindow::refreshSummary()
{
    if (store_ == nullptr || !store_->isOpen()) {
        summaryLabel_->setText(QStringLiteral("数据库不可用"));
        return;
    }
    QString error;
    const TodaySummary summary = store_->todaySummary(&error);
    if (!error.isEmpty()) {
        summaryLabel_->setText(QStringLiteral("统计失败：%1").arg(error));
        return;
    }
    summaryLabel_->setText(
        QStringLiteral("今日：%1 单 / %2 页 · 应收 %3 · 已收 %4 · 未收 %5")
            .arg(summary.jobs)
            .arg(summary.pages)
            .arg(formatCents(summary.amountCents))
            .arg(formatCents(summary.paidCents))
            .arg(formatCents(summary.unpaidCents)));
}

void MainWindow::refreshJobsTable()
{
    if (flow_ == nullptr) {
        return;
    }
    const QVector<PrintJobInfo> jobs = flow_->currentJobs();
    jobsTable_->setRowCount(jobs.size());
    for (int row = 0; row < jobs.size(); ++row) {
        const PrintJobInfo& job = jobs.at(row);
        jobsTable_->setItem(row, 0, makeItem(QString::number(job.jobId), Qt::AlignCenter));
        jobsTable_->setItem(row, 1, makeItem(job.document.isEmpty() ? QStringLiteral("（未知文档）")
                                                                   : job.document));
        jobsTable_->setItem(row, 2, makeItem(job.billablePages() > 0
                                                 ? QString::number(job.billablePages())
                                                 : QStringLiteral("待填"),
                                             Qt::AlignCenter));
        jobsTable_->setItem(row, 3, makeItem(QString::number(job.copies), Qt::AlignCenter));
        jobsTable_->setItem(row, 4, makeItem(stateText(job.state), Qt::AlignCenter));
        jobsTable_->setItem(row, 5, makeItem(job.simulated ? QStringLiteral("模拟作业")
                                                          : (job.chargeable ? QStringLiteral("收费")
                                                                            : QStringLiteral("免费")),
                                             Qt::AlignCenter));
    }
}

void MainWindow::appendLog(const QString& text)
{
    logView_->appendPlainText(text);
    writeLogToFile(text);
}

void MainWindow::writeLogToFile(const QString& text)
{
    const QString path = logFilePath();
    QFile file(path);
    // 文件过大先滚动，避免长期运行写满磁盘
    if (file.exists() && file.size() > kMaxLogBytes) {
        QFile::remove(path + QStringLiteral(".old"));
        QFile::rename(path, path + QStringLiteral(".old"));
    }
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        return;   // 日志写不进去不影响主流程
    }
    QTextStream stream(&file);
    stream << text << '\n';
}

void MainWindow::onRelaunchElevatedClicked()
{
    // 用 ShellExecuteW 的 "runas" 动词重新启动自己 —— 会弹出 UAC 确认框。
    // 之所以需要提权：暂停打印机队列（SetPrinter PRINTER_CONTROL_PAUSE）与取消
    // 任意作业（SetJob JOB_CONTROL_CANCEL）都属于管理员权限操作。
    const QString exePath = QCoreApplication::applicationFilePath();
    const QString arguments = QStringLiteral("--elevated");
    const HINSTANCE result = ShellExecuteW(
        nullptr, L"runas",
        reinterpret_cast<const wchar_t*>(exePath.utf16()),
        reinterpret_cast<const wchar_t*>(arguments.utf16()),
        nullptr, SW_SHOWNORMAL);

    // ShellExecuteW 返回值 > 32 表示成功；<= 32 是错误码（用户点了“否”通常是 5）
    if (reinterpret_cast<qintptr>(result) <= 32) {
        QMessageBox::warning(this, QStringLiteral("打印计费助手"),
                             QStringLiteral("提权启动失败（可能你点了“否”）。\n\n"
                                            "也可以关闭本程序，右键 PrintPay.exe → “以管理员身份运行”。"));
        return;
    }

    appendLog(QStringLiteral("已请求以管理员身份重启程序"));
    // 先把打印机恢复原状再退出，避免把暂停状态留给下一个进程处理
    if (flow_ != nullptr) {
        flow_->stop();
    }
    forceQuit_ = true;
    QCoreApplication::quit();
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    // 默认行为：点关闭按钮只是把窗口收进托盘，程序继续在后台监控打印。
    // 只有这样“开机自启 + 常驻任务栏”才有意义（否则一关就彻底不监控了）。
    if (!forceQuit_ && settings_.closeToTray && trayAvailable_) {
        event->ignore();
        hideToTray();
        return;
    }

    if (flow_ != nullptr) {
        flow_->stop();   // 真正退出前恢复打印机，避免客户的文件卡在队列里
    }
    if (trayIcon_ != nullptr) {
        trayIcon_->hide();   // 先把托盘图标撤掉，避免退出后残留“僵尸图标”
    }
    event->accept();
}

}  // namespace printpay
