// =============================================================================
// 文件名：src/core/job_flow.cpp
// 作用  ：打印作业状态机实现（见 job_flow.h 的说明）。
// 设计要点：
//   1) 单线程 + QTimer 轮询：打印业务量小，轮询足够灵敏，且彻底避免并发问题；
//   2) 弹窗期间用 dialogOpen_ 屏蔽轮询逻辑：弹窗是模态对话框（内部有嵌套事件循环），
//      定时器会继续触发，若不屏蔽就会出现“弹窗套弹窗”“一个作业收两次钱”这类 bug；
//   3) 需要审批时先暂停打印机队列再弹窗 —— 这样客户的作业只会躺在队列里，
//      老板不同意就不会打出来；
//   4) 每个作业号只处理一次（handledAt_ 记忆），并且在作业从队列消失时才结算，
//      页数优先取驱动上报值，取不到就交给老板在收款弹窗里手工填。
// =============================================================================
#include "core/job_flow.h"

#include <QDateTime>
#include <QSet>

#include <algorithm>

#include "core/billing.h"

namespace printpay {
namespace {

// 已处理作业的记忆时长：超过这个时间的记忆会被清理
constexpr qint64 kHandledRememberMs = 30 * 60 * 1000;
// 模拟作业的“打印耗时”：3 秒后视为打完，用于走通整条流程
constexpr qint64 kSimulatedPrintMs = 3000;
// 轮询错误日志的节流间隔，避免打印机掉线时日志刷屏
constexpr qint64 kErrorLogIntervalMs = 30000;

}  // namespace

QString Session::displayName() const
{
    if (!loggedIn) {
        return QStringLiteral("未登录（所有作业均收费）");
    }
    return isAdmin() ? QStringLiteral("%1（管理员 · 免费）").arg(userName) : userName;
}

JobFlow::JobFlow(Store* store, QObject* parent)
    : QObject(parent)
    , store_(store)
{
    timer_.setSingleShot(false);
    connect(&timer_, &QTimer::timeout, this, &JobFlow::onTick);
}

JobFlow::~JobFlow()
{
    stop();
}

void JobFlow::setSettings(const AppSettings& settings)
{
    settings_ = settings;
    if (timer_.interval() != settings_.pollIntervalMs) {
        timer_.setInterval(settings_.pollIntervalMs);
    }
}

AppSettings JobFlow::settings() const
{
    return settings_;
}

bool JobFlow::start(const QString& printerName, QString* error)
{
    stop();   // 保证可重复调用：先收拾干净再开始

    QString detail;
    if (!spooler_.attach(printerName, &detail)) {
        if (error != nullptr) {
            *error = detail;
        }
        return false;
    }

    // 开机自检：如果这台打印机停着，并且配置里记录着“是本程序暂停的”，就自动恢复，
    // 这样即使程序上次被强制结束，打印机也不会一直卡在暂停状态
    if (settings_.pausedByApp) {
        QString resumeError;
        if (spooler_.resumePrinter(&resumeError)) {
            settings_.pausedByApp = false;
            settings_.save();
            log(QStringLiteral("检测到上次退出时把打印机停在了暂停状态，已自动恢复"));
        } else {
            log(QStringLiteral("尝试恢复打印机失败：%1（可点界面上的“恢复打印机”重试）").arg(resumeError));
        }
    }
    pausedByUs_ = false;

    timer_.setInterval(settings_.pollIntervalMs);
    timer_.start();
    running_ = true;
    log(QStringLiteral("开始监控打印机：%1（轮询间隔 %2 毫秒）")
            .arg(printerName).arg(settings_.pollIntervalMs));
    emit printerPauseChanged(spooler_.isPaused());
    emit jobsChanged();
    return true;
}

void JobFlow::stop()
{
    if (timer_.isActive()) {
        timer_.stop();
    }
    // 退出前一定把被本程序暂停的打印机恢复，否则客户的文件会一直卡在队列里
    if (pausedByUs_ && spooler_.isAttached()) {
        QString error;
        if (spooler_.resumePrinter(&error)) {
            settings_.pausedByApp = false;
            settings_.save();
            log(QStringLiteral("停止监控：已恢复被本程序暂停的打印机"));
        } else {
            log(QStringLiteral("停止监控时恢复打印机失败：%1").arg(error));
        }
    }
    pausedByUs_ = false;
    spooler_.detach();
    jobs_.clear();
    running_ = false;
}

bool JobFlow::isRunning() const
{
    return running_;
}

QString JobFlow::printerName() const
{
    return spooler_.printerName();
}

bool JobFlow::printerPaused(QString* error) const
{
    if (!spooler_.isAttached()) {
        return false;
    }
    return spooler_.isPaused(error);
}

bool JobFlow::canControlPrinter() const
{
    return spooler_.isAttached() && spooler_.canControl();
}

bool JobFlow::forceResumePrinter(QString* error)
{
    if (!spooler_.isAttached()) {
        if (error != nullptr) {
            *error = QStringLiteral("尚未绑定打印机");
        }
        return false;
    }
    if (!spooler_.resumePrinter(error)) {
        return false;
    }
    if (pausedByUs_) {
        pausedByUs_ = false;
        settings_.pausedByApp = false;
        settings_.save();
    }
    log(QStringLiteral("已手动恢复打印机"));
    emit printerPauseChanged(false);
    return true;
}

void JobFlow::setSession(const Session& session)
{
    session_ = session;
    log(QStringLiteral("当前身份：%1").arg(session_.displayName()));
    emit jobsChanged();
}

Session JobFlow::session() const
{
    return session_;
}

QVector<PrintJobInfo> JobFlow::currentJobs() const
{
    QVector<PrintJobInfo> jobs;
    jobs.reserve(jobs_.size());
    for (auto it = jobs_.constBegin(); it != jobs_.constEnd(); ++it) {
        jobs.push_back(it.value());
    }
    // 按作业号升序，界面表格看起来稳定（哈希表本身无序）
    std::sort(jobs.begin(), jobs.end(), [](const PrintJobInfo& a, const PrintJobInfo& b) {
        return a.jobId < b.jobId;
    });
    return jobs;
}

void JobFlow::approveJob(quint32 jobId, bool freeOfCharge)
{
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) {
        return;   // 作业已经结束或已处理，忽略
    }
    PrintJobInfo& job = it.value();
    job.approved = true;
    job.state = JobState::Printing;
    if (freeOfCharge) {
        job.chargeable = false;   // 老板选择“同意但免费”
    }
    if (job.simulated) {
        job.simulatedFinishMs = QDateTime::currentMSecsSinceEpoch() + kSimulatedPrintMs;
    }
    log(QStringLiteral("老板已同意打印：%1%2")
            .arg(job.document.isEmpty() ? QStringLiteral("(未知文档)") : job.document,
                 freeOfCharge ? QStringLiteral("（本次免费）") : QString()));
    resumeIfNoPendingApproval();
    emit jobsChanged();
}

void JobFlow::rejectJob(quint32 jobId)
{
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) {
        return;
    }
    PrintJobInfo job = it.value();

    if (!job.simulated && spooler_.isAttached()) {
        QString error;
        if (!spooler_.cancelJob(jobId, &error)) {
            // 取消失败通常意味着作业已经打完：这种情况按正常流程处理，该收费还是要收，
            // 绝不因为“取消失败”就把客户已经拿到的成品漏掉
            log(QStringLiteral("取消作业失败：%1，将按已打印完成处理").arg(error));
            jobs_.erase(it);
            handleFinishedJob(job);
            resumeIfNoPendingApproval();
            emit jobsChanged();
            return;
        }
        log(QStringLiteral("已拒绝并取消作业：%1").arg(job.document));
    } else {
        log(QStringLiteral("已拒绝模拟作业：%1").arg(job.document));
    }

    rememberHandled(jobId, job);
    jobs_.erase(it);
    resumeIfNoPendingApproval();
    emit jobsChanged();
}

void JobFlow::finishPayment(quint32 jobId, int pages, bool paid)
{
    auto it = jobs_.find(jobId);
    if (it == jobs_.end()) {
        return;
    }
    const PrintJobInfo job = it.value();
    const int amount = amountForJob(job, pages);
    saveRecord(job, pages, amount, paid, false);
    it.value().paymentShown = true;   // 标记已处理，防止重复弹窗
    emit jobsChanged();
}

void JobFlow::simulateJob(int pages, const QString& document)
{
    PrintJobInfo job;
    job.jobId = 900000u + static_cast<quint32>(simulatedCounter_++);   // 用 900000 段避免与真实作业号冲突
    job.printerName = spooler_.printerName().isEmpty() ? QStringLiteral("(模拟打印机)")
                                                      : spooler_.printerName();
    job.document = document;
    job.userName = QStringLiteral("测试客户");
    job.machineName = QStringLiteral("本机");
    job.statusText = QStringLiteral("模拟作业");
    job.totalPages = pages > 0 ? pages : 0;
    job.printedPages = job.totalPages;
    job.copies = 1;
    job.submittedMs = QDateTime::currentMSecsSinceEpoch();
    job.simulated = true;

    log(QStringLiteral("模拟一次打印作业：%1 页（不会真的打印，仅用于测试流程）").arg(pages));
    handleNewJob(job);   // 内部会登记到 jobs_ 并按策略弹审批框
    emit jobsChanged();
}

void JobFlow::onTick()
{
    if (!running_) {
        return;
    }
    // 弹窗期间不处理：模态对话框内部有嵌套事件循环，定时器还会触发，
    // 这里直接返回，避免出现嵌套弹窗或重复收费
    if (dialogOpen_) {
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // ---- 第 1 步：模拟作业到点视为打印完成 ----
    QVector<quint32> simulatedDone;
    for (auto it = jobs_.constBegin(); it != jobs_.constEnd(); ++it) {
        const PrintJobInfo& job = it.value();
        if (job.simulated && job.state == JobState::Printing && job.simulatedFinishMs > 0 &&
            now >= job.simulatedFinishMs) {
            simulatedDone.push_back(it.key());
        }
    }
    for (quint32 id : simulatedDone) {
        const PrintJobInfo job = jobs_.value(id);
        handleFinishedJob(job);
        jobs_.remove(id);
        emit jobsChanged();
    }

    // ---- 第 2 步：轮询真实打印队列 ----
    if (spooler_.isAttached()) {
        QString error;
        const QVector<PrintJobInfo> current = spooler_.enumerateJobs(&error);
        if (!error.isEmpty()) {
            // 出问题时按节流记日志，避免刷屏
            if (now - lastErrorMs_ > kErrorLogIntervalMs || error != lastErrorText_) {
                lastErrorText_ = error;
                lastErrorMs_ = now;
                log(QStringLiteral("读取打印队列失败：%1").arg(error));
            }
        } else {
            QSet<quint32> currentIds;
            // 这里按值遍历：handleNewJob 需要一份可修改的副本（它会填充状态、收费与审批结果），
            // 而 current 是只读快照，不能直接把元素当成非 const 引用传出去
            for (PrintJobInfo job : current) {
                currentIds.insert(job.jobId);
                auto it = jobs_.find(job.jobId);
                if (it == jobs_.end()) {
                    if (recentlyHandled(job.jobId, job)) {
                        continue;   // 极端情况下的重复作业（作业号被复用），跳过避免重复收费
                    }
                    handleNewJob(job);   // 可能同步弹出审批框
                    emit jobsChanged();
                } else {
                    // 已跟踪的作业：只刷新系统侧字段，保留本程序维护的状态与标记
                    PrintJobInfo& tracked = it.value();
                    tracked.totalPages = job.totalPages;
                    tracked.printedPages = job.printedPages;
                    tracked.statusText = job.statusText;
                    tracked.windowsStatus = job.windowsStatus;
                    if (tracked.state == JobState::Monitoring) {
                        tracked.state = JobState::Printing;
                    }
                }
            }

            // 从队列里消失的作业 => 打印完成
            QVector<quint32> gone;
            for (auto it = jobs_.constBegin(); it != jobs_.constEnd(); ++it) {
                if (it.value().simulated) {
                    continue;   // 模拟作业由第 1 步处理
                }
                if (!currentIds.contains(it.key())) {
                    gone.push_back(it.key());
                }
            }
            for (quint32 id : gone) {
                const PrintJobInfo job = jobs_.value(id);
                handleFinishedJob(job);
                jobs_.remove(id);
                emit jobsChanged();
            }
        }
    }

    // ---- 第 3 步：清理过期的“已处理”记忆 ----
    for (auto it = handledAt_.begin(); it != handledAt_.end();) {
        if (now - it.value() > kHandledRememberMs) {
            handledDocs_.remove(it.key());
            it = handledAt_.erase(it);
        } else {
            ++it;
        }
    }
}

void JobFlow::handleNewJob(PrintJobInfo& job)
{
    const bool admin = session_.isAdmin();
    // 收费与审批策略：管理员与其他人可以分别配置
    job.chargeable = admin ? settings_.chargeAdmin : settings_.chargeOthers;
    const bool needApproval = admin ? settings_.requireApprovalForAdmin
                                    : settings_.requireApprovalForOthers;
    job.approved = !needApproval;
    job.state = needApproval ? JobState::WaitingApprove : JobState::Printing;

    log(QStringLiteral("发现新作业：作业号 %1，文档「%2」，提交者 %3，驱动页数 %4，份数 %5，%6")
            .arg(job.jobId)
            .arg(job.document.isEmpty() ? QStringLiteral("未知") : job.document)
            .arg(job.userName.isEmpty() ? QStringLiteral("未知") : job.userName)
            .arg(job.totalPages)
            .arg(job.copies)
            .arg(job.chargeable ? QStringLiteral("需收费") : QStringLiteral("免费")));

    if (!needApproval) {
        // 不需要审批：直接放行（管理员登录时就是这条路径）
        if (job.simulated) {
            job.simulatedFinishMs = QDateTime::currentMSecsSinceEpoch() + kSimulatedPrintMs;
        }
        jobs_.insert(job.jobId, job);
        return;
    }

    // 需要审批：先暂停打印机队列，确保客户的文件不会直接打出来
    if (!job.simulated && spooler_.isAttached() && !pausedByUs_) {
        QString error;
        if (spooler_.pausePrinter(&error)) {
            pausedByUs_ = true;
            settings_.pausedByApp = true;   // 记进配置，程序异常退出后下次启动能自动恢复
            settings_.save();
            emit printerPauseChanged(true);
            log(QStringLiteral("已暂停打印机，等待老板同意后再打印"));
        } else if (!spooler_.canControl()) {
            // 权限不足：审批拦不住打印，必须明确告知，不能让老板以为“已经拦住了”
            log(QStringLiteral("⚠ 无法暂停打印机（%1）。本次审批只能在事后提示，"
                               "文件可能已经开始打印；请点界面上的“以管理员身份重启”后重试。")
                    .arg(error));
        } else {
            log(QStringLiteral("暂停打印机失败：%1 —— 请手动检查打印机状态").arg(error));
        }
    }

    // 关键：先把作业登记进 jobs_，再弹审批框。
    // 因为弹窗是模态的，老板点“同意/拒绝”时回调会去 jobs_ 里找这个作业；
    // 若先弹窗后登记，回调就会找不到作业，导致点了没反应（典型的时序 bug）。
    jobs_.insert(job.jobId, job);

    dialogOpen_ = true;
    emit approvalRequested(job, job.chargeable);
    dialogOpen_ = false;
}

void JobFlow::handleFinishedJob(const PrintJobInfo& job)
{
    if (job.state == JobState::Canceled || job.paymentShown) {
        return;   // 已取消或已结算过
    }
    rememberHandled(job.jobId, job);

    const int pages = job.billablePages();

    // 情况一：免费（管理员登录或老板选了“同意但免费”）—— 不弹收款码，只记流水
    if (!job.chargeable) {
        log(QStringLiteral("作业完成（免费）：%1，%2 页")
                .arg(job.document.isEmpty() ? QStringLiteral("未知文档") : job.document)
                .arg(pages));
        saveRecord(job, pages, 0, true, true);
        return;
    }

    // 情况二：需要收费但配置关了自动收款弹窗 —— 只记一笔“未收款”，由老板自己核对
    if (!settings_.autoPaymentDialog) {
        const int amount = amountForJob(job, pages);
        log(QStringLiteral("作业完成（未弹收款码）：%1，%2 页，应收 %3")
                .arg(job.document).arg(pages).arg(formatCents(amount)));
        saveRecord(job, pages, amount, false, false);
        return;
    }

    // 情况三：正常收费流程 —— 弹收款码；老板确认后回调 finishPayment() 才写流水
    const int amount = amountForJob(job, pages);
    log(QStringLiteral("作业完成，弹出收款码：%1，%2 页，应收 %3")
            .arg(job.document.isEmpty() ? QStringLiteral("未知文档") : job.document)
            .arg(pages)
            .arg(formatCents(amount)));
    dialogOpen_ = true;
    emit paymentRequested(job, amount);
    dialogOpen_ = false;
}

void JobFlow::saveRecord(const PrintJobInfo& job, int pages, int amountCents,
                         bool paid, bool freeOfCharge)
{
    if (store_ == nullptr || !store_->isOpen()) {
        log(QStringLiteral("数据库不可用，本次流水未能保存"));
        return;
    }
    PrintRecord record;
    record.tsMs = QDateTime::currentMSecsSinceEpoch();
    record.printerName = job.printerName;
    record.document = job.document;
    record.owner = job.userName;
    record.account = session_.loggedIn ? session_.userName : QStringLiteral("未登录");
    record.pages = pages;
    record.copies = job.copies;
    record.amountCents = amountCents;
    record.paid = paid;
    record.freeOfCharge = freeOfCharge;

    QString error;
    if (!store_->addRecord(record, &error)) {
        log(QStringLiteral("写入流水失败：%1").arg(error));
        return;
    }
    log(QStringLiteral("流水已记录：%1 页 × %2 份，金额 %3，%4")
            .arg(pages)
            .arg(job.copies)
            .arg(formatCents(amountCents))
            .arg(freeOfCharge ? QStringLiteral("免费") : (paid ? QStringLiteral("已收款")
                                                              : QStringLiteral("未收款"))));
}

int JobFlow::amountForJob(const PrintJobInfo& job, int pages) const
{
    if (!job.chargeable) {
        return 0;
    }
    return computeAmountCents(pages, job.copies, settings_.pricePerPageCents, settings_.minChargeCents);
}

void JobFlow::resumeIfNoPendingApproval()
{
    for (auto it = jobs_.constBegin(); it != jobs_.constEnd(); ++it) {
        if (it.value().state == JobState::WaitingApprove) {
            return;   // 还有别的客户在等审批，先别恢复
        }
    }
    if (pausedByUs_ && spooler_.isAttached()) {
        QString error;
        if (spooler_.resumePrinter(&error)) {
            pausedByUs_ = false;
            settings_.pausedByApp = false;
            settings_.save();
            emit printerPauseChanged(false);
            log(QStringLiteral("已恢复打印机，作业开始打印"));
        } else {
            log(QStringLiteral("恢复打印机失败：%1（可点界面上的“恢复打印机”重试）").arg(error));
        }
    }
}

bool JobFlow::recentlyHandled(quint32 jobId, const PrintJobInfo& job) const
{
    auto it = handledAt_.find(jobId);
    if (it == handledAt_.end()) {
        return false;
    }
    // 作业号被系统复用属极少数情况，再用文档名比对一次，避免误伤正常的新作业
    const QString handledDoc = handledDocs_.value(jobId);
    if (handledDoc.isEmpty() || job.document.isEmpty() || handledDoc == job.document) {
        return true;
    }
    return false;
}

void JobFlow::rememberHandled(quint32 jobId, const PrintJobInfo& job)
{
    handledAt_.insert(jobId, QDateTime::currentMSecsSinceEpoch());
    handledDocs_.insert(jobId, job.document);
}

void JobFlow::log(const QString& text)
{
    emit logMessage(QStringLiteral("[%1] %2")
                        .arg(QDateTime::currentDateTime().toString(QStringLiteral("MM-dd hh:mm:ss")), text));
}

}  // namespace printpay
