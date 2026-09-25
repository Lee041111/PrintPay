// =============================================================================
// 文件名：src/core/job_flow.h
// 作用  ：打印作业的业务状态机 —— 本程序的大脑。职责：
//           1) 定时轮询打印队列，发现新作业；
//           2) 按“当前登录账号 + 策略配置”决定这个作业是否收费、是否需要老板审批；
//           3) 需要审批时先暂停打印机队列（保证客户的东西不会直接打出来），
//              界面弹出审批窗，老板同意后再恢复队列；
//           4) 作业从队列消失即视为打印完成，通知界面弹出收款码；
//           5) 把每次打印写进流水表（页数、金额、是否已收款）。
// 位置  ：被 MainWindow 持有；所有信号都由界面接收并弹窗，逻辑与界面严格分离。
// 注意  ：1) 全程在主线程用 QTimer 轮询，不引入多线程，从根上避免并发 bug；
//         2) 弹窗期间用 dialogOpen_ 锁住轮询逻辑，防止定时器重入导致重复弹窗；
//         3) 同一作业号只处理一次（handledIds_ 记录已处理作业，防止重复收费）。
// =============================================================================
#pragma once

#include <QHash>
#include <QObject>
#include <QTimer>

#include "core/print_job.h"
#include "core/settings.h"
#include "core/spooler.h"
#include "core/store.h"

namespace printpay {

// 当前登录会话
struct Session {
    bool loggedIn = false;      // 是否已登录（未登录也会监控并收费）
    QString userName;           // 登录账号
    AccountRole role = AccountRole::User;

    bool isAdmin() const { return loggedIn && role == AccountRole::Admin; }
    QString displayName() const;
};

class JobFlow : public QObject {
    Q_OBJECT

public:
    explicit JobFlow(Store* store, QObject* parent = nullptr);
    ~JobFlow() override;

    // 更新配置（设置界面保存后调用）
    void setSettings(const AppSettings& settings);
    AppSettings settings() const;

    // 开始监控某台打印机：绑定句柄 + 启动轮询定时器。
    // 若绑定的这台打印机之前被本程序暂停过（异常退出留下的状态），会自动恢复。
    bool start(const QString& printerName, QString* error = nullptr);

    // 停止监控，并在退出前把“被本程序暂停的打印机”恢复，避免打印机一直停着。
    void stop();

    bool isRunning() const;
    QString printerName() const;

    // 当前打印机是否处于暂停状态（界面显示用）
    bool printerPaused(QString* error = nullptr) const;

    // 是否具备“暂停打印机 / 取消作业”的权限（= 程序以管理员身份运行）。
    // 为 false 时：统计与收费照常，但“打印前先拦住”无法生效，界面会提示重启提权。
    bool canControlPrinter() const;

    // 手动恢复打印机（界面上给老板的保险按钮）
    bool forceResumePrinter(QString* error = nullptr);

    // ---- 会话 ----
    void setSession(const Session& session);
    Session session() const;

    // 当前正在跟踪的作业（界面表格显示用；按作业号升序）
    QVector<PrintJobInfo> currentJobs() const;

    // ---- 界面回调（弹窗处理完通知状态机）----
    // 老板同意打印。freeOfCharge 为 true 表示这次给熟人免费（同意但免费）。
    void approveJob(quint32 jobId, bool freeOfCharge);
    // 老板拒绝：把队列里的作业删掉。
    void rejectJob(quint32 jobId);
    // 收款弹窗结束：pages 允许老板手工修正（驱动没上报页数时），paid 表示是否已收款。
    void finishPayment(quint32 jobId, int pages, bool paid);

    // ---- 测试用：模拟一次打印作业（不碰真实打印机，用于验证整条流程）----
    void simulateJob(int pages, const QString& document = QStringLiteral("测试文档.pdf"));

signals:
    // 需要老板审批（界面弹审批窗）
    void approvalRequested(const PrintJobInfo& job, bool willCharge);
    // 打印完成需要收款（界面弹收款码）
    void paymentRequested(const PrintJobInfo& job, int amountCents);
    // 日志（界面显示 + 写文件）
    void logMessage(const QString& text);
    // 作业列表或状态变化（界面刷新）
    void jobsChanged();
    // 打印机暂停状态变化
    void printerPauseChanged(bool paused);

private slots:
    void onTick();

private:
    // 发现新作业：决定收费与否、是否需要审批
    void handleNewJob(PrintJobInfo& job);
    // 作业从队列消失：视为打印完成，必要时弹收款码
    void handleFinishedJob(const PrintJobInfo& job);
    // 把流水写进数据库（失败不影响界面，只记日志）
    void saveRecord(const PrintJobInfo& job, int pages, int amountCents, bool paid, bool freeOfCharge);
    // 计算某作业应收金额（分）
    int amountForJob(const PrintJobInfo& job, int pages) const;
    // 如果当前没有人还在等审批，就恢复打印机
    void resumeIfNoPendingApproval();
    // 判断这个作业号最近是否已经处理过（防止极端情况下重复收费）
    bool recentlyHandled(quint32 jobId, const PrintJobInfo& job) const;
    void rememberHandled(quint32 jobId, const PrintJobInfo& job);
    // 内部日志
    void log(const QString& text);

    Store* store_ = nullptr;
    Spooler spooler_;
    AppSettings settings_;
    Session session_;
    QTimer timer_;

    QHash<quint32, PrintJobInfo> jobs_;          // 当前队列中的作业（作业号为键）
    QHash<quint32, qint64> handledAt_;           // 已处理作业号 -> 处理时间（防重复）
    QHash<quint32, QString> handledDocs_;        // 已处理作业号 -> 文档名（防重复时比对）
    bool dialogOpen_ = false;                    // 弹窗期间暂停轮询逻辑，防止重入
    bool pausedByUs_ = false;                    // 打印机暂停是否由本程序造成
    bool running_ = false;
    int simulatedCounter_ = 1;                   // 模拟作业号计数
    QString lastErrorText_;                      // 上一次轮询错误（用于日志节流）
    qint64 lastErrorMs_ = 0;                     // 上一次记录错误日志的时间
};

}  // namespace printpay
