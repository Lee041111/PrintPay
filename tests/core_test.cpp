// =============================================================================
// 文件名：tests/core_test.cpp
// 作用  ：控制台自测程序，覆盖：
//           1) 计费算法（解析、最低消费、份数、异常输入）；
//           2) 数据库（建表、默认管理员、登录、改密、流水与今日汇总）；
//           3) 打印后台访问（枚举打印机、暂停/恢复、枚举队列）；
//           4) 业务状态机（模拟作业 → 审批 → 完成 → 收款 → 写流水，三种身份各测一遍）；
//           5) 【可选】对真实打印队列做集成测试：暂停打印机 → 提交一个文本作业
//              → 用本程序读取队列（验证页数/文档名/份数）→ 取消作业 → 恢复打印机。
//              全程打印机处于暂停状态且作业被取消，不会真的出纸。
// 运行  ：printpay_core_test                       只跑前 4 项（不碰真实打印机）
//         printpay_core_test --spooler [打印机名]   额外跑第 5 项集成测试
// =============================================================================
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QProcess>
#include <QTextStream>
#include <QTimer>

#include <cstdio>

#include "core/billing.h"
#include "core/job_flow.h"
#include "core/settings.h"
#include "core/spooler.h"
#include "core/store.h"

using namespace printpay;

namespace {

int g_checkCount = 0;
int g_failCount = 0;

#define CHECK(condition)                                                \
    do {                                                                \
        ++g_checkCount;                                                 \
        if (!(condition)) {                                             \
            std::printf("[失败] 第 %d 行: %s\n", __LINE__, #condition);  \
            ++g_failCount;                                              \
        }                                                               \
    } while (0)

void section(const char* name)
{
    std::printf("\n---- %s ----\n", name);
}

QString tempPath(const QString& name)
{
    return QDir(QDir::tempPath()).filePath(name);
}

// 跑事件循环若干毫秒（让 JobFlow 的轮询定时器有机会触发）
void waitFor(int ms)
{
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

// ---------------------------------------------------------------------------
// 1. 计费算法
// ---------------------------------------------------------------------------
void testBilling()
{
    section("计费算法");

    CHECK(parseYuanToCents(QStringLiteral("0.1")) == 10);
    CHECK(parseYuanToCents(QStringLiteral("0.10")) == 10);
    CHECK(parseYuanToCents(QStringLiteral("1")) == 100);
    CHECK(parseYuanToCents(QStringLiteral("0.05")) == 5);
    CHECK(parseYuanToCents(QStringLiteral("99.99")) == 9999);
    CHECK(parseYuanToCents(QStringLiteral(" 0.2 ")) == 20);   // 前后空格要能容忍
    // 非法输入必须被拒绝（返回 -1），绝不能当成 0 元
    CHECK(parseYuanToCents(QStringLiteral("")) == -1);
    CHECK(parseYuanToCents(QStringLiteral("abc")) == -1);
    CHECK(parseYuanToCents(QStringLiteral("-1")) == -1);
    CHECK(parseYuanToCents(QStringLiteral("0.123")) == -1);   // 超过两位小数
    CHECK(parseYuanToCents(QStringLiteral("1.2.3")) == -1);

    CHECK(formatCents(10) == QStringLiteral("0.10 元"));
    CHECK(formatCents(0) == QStringLiteral("0.00 元"));
    CHECK(formatCents(9999) == QStringLiteral("99.99 元"));
    CHECK(formatCents(-5) == QStringLiteral("0.00 元"));      // 负数被夹到 0

    CHECK(computeAmountCents(10, 1, 10, 10) == 100);          // 10 页 × 0.10 元
    CHECK(computeAmountCents(3, 2, 10, 10) == 60);            // 3 页 × 2 份 × 0.10 元
    CHECK(computeAmountCents(1, 1, 10, 20) == 20);            // 低于最低消费时按最低消费
    CHECK(computeAmountCents(0, 1, 10, 10) == 0);             // 页数未知 → 0，交给老板手填
    CHECK(computeAmountCents(5, 0, 10, 0) == 50);             // 份数 0 按 1 份
    CHECK(computeAmountCents(5, -2, 10, 0) == 50);            // 份数为负按 1 份
    CHECK(computeAmountCents(1000000, 10, 100, 0) > 0);       // 极端页数不溢出成负数
}

// ---------------------------------------------------------------------------
// 2. 数据库：账号与流水
// ---------------------------------------------------------------------------
void testStore()
{
    section("数据库：账号与流水");

    const QString path = tempPath(QStringLiteral("printpay_test_store.db"));
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));

    Store store;
    QString error;
    CHECK(store.open(path, &error));
    CHECK(error.isEmpty());
    CHECK(store.isOpen());

    QString createdPassword;
    CHECK(store.ensureSchema(&createdPassword, &error));
    CHECK(createdPassword == QStringLiteral("admin123"));   // 首次运行创建默认管理员
    CHECK(store.accountCount() == 1);

    QString secondPassword;
    CHECK(store.ensureSchema(&secondPassword, &error));
    CHECK(secondPassword.isEmpty());                        // 第二次不会再创建
    CHECK(store.accountCount() == 1);

    AccountRole role = AccountRole::User;
    CHECK(store.authenticate(QStringLiteral("admin"), QStringLiteral("admin123"), &role, &error));
    CHECK(role == AccountRole::Admin);
    CHECK(!store.authenticate(QStringLiteral("admin"), QStringLiteral("wrong"), &role, &error));
    CHECK(!store.authenticate(QStringLiteral("nobody"), QStringLiteral("x"), &role, &error));

    CHECK(store.addAccount(QStringLiteral("user1"), QStringLiteral("pass123"), AccountRole::User, &error));
    CHECK(store.authenticate(QStringLiteral("user1"), QStringLiteral("pass123"), &role, &error));
    CHECK(role == AccountRole::User);
    CHECK(!store.addAccount(QStringLiteral("user1"), QStringLiteral("pass123"), AccountRole::User, &error));
    CHECK(!store.addAccount(QStringLiteral("u2"), QStringLiteral("short"), AccountRole::User, &error));

    CHECK(store.changePassword(QStringLiteral("user1"), QStringLiteral("newpass1"), &error));
    CHECK(store.authenticate(QStringLiteral("user1"), QStringLiteral("newpass1"), &role, &error));
    CHECK(!store.authenticate(QStringLiteral("user1"), QStringLiteral("pass123"), &role, &error));
    CHECK(!store.changePassword(QStringLiteral("user1"), QStringLiteral("123"), &error));   // 太短

    PrintRecord record;
    record.printerName = QStringLiteral("测试打印机");
    record.document = QStringLiteral("测试文档.pdf");
    record.owner = QStringLiteral("test");
    record.account = QStringLiteral("user1");
    record.pages = 10;
    record.copies = 2;
    record.amountCents = 200;
    record.paid = true;
    CHECK(store.addRecord(record, &error));

    record.pages = 5;
    record.copies = 1;
    record.amountCents = 50;
    record.paid = false;
    CHECK(store.addRecord(record, &error));

    const TodaySummary summary = store.todaySummary(&error);
    CHECK(summary.jobs == 2);
    CHECK(summary.pages == 15);
    CHECK(summary.amountCents == 250);
    CHECK(summary.paidCents == 200);
    CHECK(summary.unpaidCents == 50);

    const QVector<PrintRecord> records = store.recentRecords(10, &error);
    CHECK(records.size() == 2);
    CHECK(records.first().amountCents == 50);   // 最近的在前

    store.close();
    CHECK(!store.isOpen());
    QFile::remove(path);
    QFile::remove(path + QStringLiteral("-wal"));
    QFile::remove(path + QStringLiteral("-shm"));
}

// ---------------------------------------------------------------------------
// 3. 打印后台访问：枚举打印机、暂停/恢复、枚举队列
// ---------------------------------------------------------------------------
void testSpoolerBasics()
{
    section("打印后台访问");

    QString error;
    const QVector<PrinterInfo> printers = Spooler::listPrinters(&error);
    std::printf("本机发现 %d 台打印机：\n", printers.size());
    for (const PrinterInfo& printer : printers) {
        // 统一用 toUtf8()：源文件是 UTF-8，控制台输出也是 UTF-8，避免中英混排乱码
        std::printf("   %-30s 端口=%-24s %s\n",
                    printer.name.toUtf8().constData(),
                    printer.port.toUtf8().constData(),
                    printer.paused ? "已暂停" : "就绪");
    }
    CHECK(!printers.isEmpty());
    CHECK(error.isEmpty());
    if (printers.isEmpty()) {
        return;
    }

    // 优先挑虚拟打印机做暂停/恢复往返测试（虚拟打印机不会真的出纸）
    QString target;
    for (const PrinterInfo& printer : printers) {
        if (printer.name.contains(QStringLiteral("PDF")) ||
            printer.name.contains(QStringLiteral("OneNote"))) {
            target = printer.name;
            break;
        }
    }
    if (target.isEmpty()) {
        target = printers.first().name;
    }
    std::printf("用「%s」做暂停/恢复测试\n", target.toUtf8().constData());

    Spooler spooler;
    CHECK(spooler.attach(target, &error));
    CHECK(spooler.isAttached());
    CHECK(!spooler.printerName().isEmpty());
    std::printf("打开打印机权限：%s\n",
                spooler.canControl() ? "完整（可暂停/取消作业）"
                                     : "只读（暂停/取消作业需要管理员权限）");

    const bool pausedBefore = spooler.isPaused(&error);
    CHECK(error.isEmpty());

    // 暂停/恢复需要管理员权限：普通权限下这是“跳过”而不是“失败”，
    // 否则会让人误以为程序有 bug（把权限问题说清楚更有用）
    if (!spooler.canControl()) {
        std::printf("【跳过】未以管理员身份运行，无法测试暂停/恢复；"
                    "如需验证请用管理员身份运行本测试\n");
    } else {
        CHECK(spooler.pausePrinter(&error));
        CHECK(error.isEmpty());
        CHECK(spooler.isPaused(&error));
        CHECK(spooler.resumePrinter(&error));
        CHECK(!spooler.isPaused(&error));
        if (pausedBefore) {
            CHECK(spooler.pausePrinter(&error));
            std::printf("（该打印机原本处于暂停状态，已按原样恢复）\n");
        }
    }

    // 枚举队列：此刻通常为空，重点是不能报错、不能崩
    const QVector<PrintJobInfo> jobs = spooler.enumerateJobs(&error);
    CHECK(error.isEmpty());
    std::printf("当前队列作业数：%d\n", jobs.size());

    // 状态文本与错误码翻译不能崩、不能返回空
    CHECK(!Spooler::printerStatusText(0).isEmpty());
    CHECK(!Spooler::printerStatusText(0x00000001u).isEmpty());   // PRINTER_STATUS_PAUSED
    CHECK(Spooler::printerStatusText(0x00000001u).contains(QStringLiteral("暂停")));
    CHECK(!Spooler::errorText(5).isEmpty());

    spooler.detach();
    CHECK(!spooler.isAttached());
}

// ---------------------------------------------------------------------------
// 4. 业务状态机：三种身份各走一遍完整流程
// ---------------------------------------------------------------------------
void testJobFlow()
{
    section("业务状态机（模拟作业，不碰真实打印机）");

    const QString path = tempPath(QStringLiteral("printpay_test_flow.db"));
    QFile::remove(path);

    Store store;
    QString error;
    CHECK(store.open(path, &error));
    CHECK(store.ensureSchema(nullptr, &error));

    JobFlow flow(&store);
    AppSettings settings;
    settings.printerName = QStringLiteral("Microsoft Print to PDF");   // 只用于绑定，不会真的打印
    settings.pricePerPageCents = 10;      // 0.10 元/页
    settings.minChargeCents = 10;
    settings.chargeOthers = true;
    settings.chargeAdmin = false;         // 管理员免费
    settings.requireApprovalForOthers = true;
    settings.requireApprovalForAdmin = false;
    settings.autoPaymentDialog = true;
    settings.pollIntervalMs = 200;        // 测试时轮询快一点
    flow.setSettings(settings);

    int approvalCount = 0;
    int paymentCount = 0;
    int lastAmount = -1;
    int lastPages = -1;

    // 场景 A：未登录 → 需要审批、需要收费
    auto approvalA = QObject::connect(&flow, &JobFlow::approvalRequested,
                                      [&](const PrintJobInfo& job, bool willCharge) {
        ++approvalCount;
        CHECK(job.simulated);
        CHECK(willCharge);
        CHECK(job.totalPages == 7);
        flow.approveJob(job.jobId, false);      // 相当于老板点“同意打印”
    });
    auto paymentA = QObject::connect(&flow, &JobFlow::paymentRequested,
                                     [&](const PrintJobInfo& job, int amountCents) {
        ++paymentCount;
        CHECK(amountCents == 70);               // 7 页 × 0.10 元
        lastAmount = amountCents;
        lastPages = job.billablePages();
        flow.finishPayment(job.jobId, job.billablePages(), true);   // 相当于老板点“已完成付款”
    });

    flow.setSession(Session());                 // 未登录
    CHECK(!flow.isRunning());
    CHECK(flow.start(settings.printerName, &error));
    CHECK(error.isEmpty());
    CHECK(flow.isRunning());
    CHECK(flow.printerName() == settings.printerName);

    flow.simulateJob(7, QStringLiteral("自测文档.pdf"));
    CHECK(flow.currentJobs().size() == 1);      // 已登记进作业表
    waitFor(4000);                              // 等定时器把模拟作业标记为“打印完成”

    CHECK(approvalCount == 1);
    CHECK(paymentCount == 1);
    CHECK(lastAmount == 70);
    CHECK(lastPages == 7);
    CHECK(flow.currentJobs().isEmpty());        // 完成后从作业表移除

    TodaySummary summary = store.todaySummary(&error);
    CHECK(summary.jobs == 1);
    CHECK(summary.pages == 7);
    CHECK(summary.amountCents == 70);
    CHECK(summary.paidCents == 70);
    CHECK(summary.unpaidCents == 0);
    QObject::disconnect(approvalA);
    QObject::disconnect(paymentA);

    // 场景 B：管理员登录 → 不审批、不收费
    approvalCount = 0;
    paymentCount = 0;
    auto approvalB = QObject::connect(&flow, &JobFlow::approvalRequested,
                                      [&](const PrintJobInfo&, bool) { ++approvalCount; });
    auto paymentB = QObject::connect(&flow, &JobFlow::paymentRequested,
                                     [&](const PrintJobInfo&, int) { ++paymentCount; });

    Session admin;
    admin.loggedIn = true;
    admin.userName = QStringLiteral("admin");
    admin.role = AccountRole::Admin;
    flow.setSession(admin);
    CHECK(flow.session().isAdmin());

    flow.simulateJob(12, QStringLiteral("老板自己的文档.pdf"));
    waitFor(4000);

    CHECK(approvalCount == 0);                  // 管理员不需要审批
    CHECK(paymentCount == 0);                   // 管理员不收费
    summary = store.todaySummary(&error);
    CHECK(summary.jobs == 2);
    CHECK(summary.pages == 19);                 // 7 + 12
    CHECK(summary.amountCents == 70);           // 免费那笔金额为 0
    QObject::disconnect(approvalB);
    QObject::disconnect(paymentB);

    // 场景 C：未登录 + 老板拒绝 → 不产生任何流水
    approvalCount = 0;
    paymentCount = 0;
    auto approvalC = QObject::connect(&flow, &JobFlow::approvalRequested,
                                      [&](const PrintJobInfo& job, bool) {
        ++approvalCount;
        flow.rejectJob(job.jobId);              // 相当于老板点“拒绝并取消打印”
    });
    auto paymentC = QObject::connect(&flow, &JobFlow::paymentRequested,
                                     [&](const PrintJobInfo&, int) { ++paymentCount; });

    flow.setSession(Session());
    flow.simulateJob(5, QStringLiteral("被拒绝的文档.pdf"));
    waitFor(4000);

    CHECK(approvalCount == 1);
    CHECK(paymentCount == 0);                   // 被拒绝的作业绝不能弹收款码
    summary = store.todaySummary(&error);
    CHECK(summary.jobs == 2);                   // 仍然只有前两笔
    QObject::disconnect(approvalC);
    QObject::disconnect(paymentC);

    // 场景 D：未登录 + 老板同意但本次免费 → 记一笔免费流水
    approvalCount = 0;
    paymentCount = 0;
    auto approvalD = QObject::connect(&flow, &JobFlow::approvalRequested,
                                      [&](const PrintJobInfo& job, bool) {
        ++approvalCount;
        flow.approveJob(job.jobId, true);       // 相当于老板点“同意（本次免费）”
    });
    auto paymentD = QObject::connect(&flow, &JobFlow::paymentRequested,
                                     [&](const PrintJobInfo&, int) { ++paymentCount; });

    flow.simulateJob(3, QStringLiteral("熟人的文档.pdf"));
    waitFor(4000);

    CHECK(approvalCount == 1);
    CHECK(paymentCount == 0);                   // 免费不弹收款码
    summary = store.todaySummary(&error);
    CHECK(summary.jobs == 3);
    CHECK(summary.pages == 22);                 // 19 + 3
    CHECK(summary.amountCents == 70);           // 免费笔不产生金额
    QObject::disconnect(approvalD);
    QObject::disconnect(paymentD);

    flow.stop();
    CHECK(!flow.isRunning());
    store.close();
    QFile::remove(path);
}

// ---------------------------------------------------------------------------
// 5.（可选）真实打印队列集成测试
// ---------------------------------------------------------------------------
bool spoolTextJob(const QString& printerName, QString* error)
{
    const QString tmpFile = tempPath(QStringLiteral("printpay_spool_test.txt"));
    QFile file(tmpFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        *error = QStringLiteral("无法写入测试文件");
        return false;
    }
    QTextStream stream(&file);
    stream << "PrintPay integration test page\n"
           << "line 2\n"
           << "line 3\n";
    file.close();

    // Windows PowerShell 自带 Out-Printer，可把文本送进指定打印机的队列
    QProcess process;
    process.start(QStringLiteral("powershell"),
                  QStringList{ QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                               QStringLiteral("Get-Content -Path '%1' | Out-Printer -Name '%2'")
                                   .arg(tmpFile, printerName) });
    if (!process.waitForFinished(30000)) {
        process.kill();
        *error = QStringLiteral("提交打印作业超时");
        return false;
    }
    if (process.exitCode() != 0) {
        *error = QStringLiteral("Out-Printer 失败：%1")
                     .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return false;
    }
    return true;
}

void testRealSpool(const QString& requested)
{
    section("真实打印队列集成测试（不会出纸）");

    // 选一台“最安全”的打印机做测试：
    //   1) 端口为 nul: 的打印机（如 OneNote Desktop）—— 作业被暂停在队列里，
    //      即使不小心放行也只会写向虚空，不会出纸、不会弹窗；
    //   2) 名字带 PDF 的虚拟打印机；
    //   3) 退化为第一台打印机。
    QString printerName = requested;
    if (printerName.isEmpty() || printerName == QStringLiteral("auto")) {
        QString listError;
        const QVector<PrinterInfo> printers = Spooler::listPrinters(&listError);
        for (const PrinterInfo& printer : printers) {
            if (printer.port == QStringLiteral("nul:")) {
                printerName = printer.name;
                break;
            }
        }
        if (printerName.isEmpty()) {
            for (const PrinterInfo& printer : printers) {
                if (printer.name.contains(QStringLiteral("PDF"))) {
                    printerName = printer.name;
                    break;
                }
            }
        }
        if (printerName.isEmpty() && !printers.isEmpty()) {
            printerName = printers.first().name;
        }
        if (printerName.isEmpty()) {
            std::printf("跳过：本机没有可用打印机\n");
            return;
        }
    }
    std::printf("目标打印机：%s\n", printerName.toUtf8().constData());

    Spooler spooler;
    QString error;
    if (!spooler.attach(printerName, &error)) {
        std::printf("跳过：%s\n", error.toUtf8().constData());
        ++g_failCount;
        return;
    }

    // 暂停/取消作业都需要管理员权限：没有权限就直接说明并跳过，
    // 不要伪装成“通过”，也不要报一堆看不懂的失败
    if (!spooler.canControl()) {
        std::printf("【跳过】当前不是管理员权限（%s），无法暂停打印机做集成测试。\n"
                    "如需完整验证，请用管理员身份运行：\n"
                    "    printpay_core_test.exe --spooler \"%s\"\n",
                    error.toUtf8().constData(), printerName.toUtf8().constData());
        return;
    }

    const bool wasPaused = spooler.isPaused(&error);
    // 关键：先暂停打印机，作业只会躺在队列里，既不会打到纸上，也不会弹出“另存为”
    CHECK(spooler.pausePrinter(&error));
    CHECK(spooler.isPaused(&error));

    if (!spoolTextJob(printerName, &error)) {
        std::printf("提交作业失败：%s\n", error.toUtf8().constData());
        ++g_failCount;
        spooler.resumePrinter(&error);
        if (wasPaused) {
            spooler.pausePrinter(&error);
        }
        return;
    }

    // 轮询队列，最多等 10 秒（间隔 50 毫秒：作业可能瞬间完成，要抓得住）
    QVector<PrintJobInfo> jobs;
    for (int i = 0; i < 200; ++i) {
        jobs = spooler.enumerateJobs(&error);
        CHECK(error.isEmpty());
        if (!jobs.isEmpty()) {
            break;
        }
        waitFor(50);
    }

    CHECK(!jobs.isEmpty());
    if (!jobs.isEmpty()) {
        const PrintJobInfo& job = jobs.first();
        std::printf("读到作业：作业号=%u 文档=%s 提交者=%s 页数=%d 份数=%d 状态=%s\n",
                    job.jobId,
                    job.document.toUtf8().constData(),
                    job.userName.toUtf8().constData(),
                    job.totalPages, job.copies,
                    job.statusText.toUtf8().constData());
        CHECK(job.totalPages >= 0);          // 页数由驱动上报，可能为 0（程序会提示手工填）
        CHECK(job.copies >= 1);
        CHECK(job.jobId > 0);
    }

    // 取消作业并恢复打印机，绝不留痕
    for (const PrintJobInfo& job : jobs) {
        CHECK(spooler.cancelJob(job.jobId, &error));
    }
    CHECK(spooler.resumePrinter(&error));
    if (wasPaused) {
        CHECK(spooler.pausePrinter(&error));   // 原本就暂停的话恢复原状
    }
    waitFor(500);
    const QVector<PrintJobInfo> after = spooler.enumerateJobs(&error);
    std::printf("清理后队列作业数：%d（应为 0）\n", after.size());
}

}  // namespace

int main(int argc, char* argv[])
{
    QCoreApplication application(argc, argv);

    std::printf("==== 打印计费助手 核心自测 ====\n");

    testBilling();
    testStore();
    testSpoolerBasics();
    testJobFlow();

    for (int i = 1; i < argc; ++i) {
        if (QString::fromLocal8Bit(argv[i]) == QStringLiteral("--spooler")) {
            // 不指定打印机时传空串，由测试自己挑一台最安全的（通常是 nul: 端口的虚拟打印机）
            QString printer;
            if (i + 1 < argc) {
                printer = QString::fromLocal8Bit(argv[i + 1]);
            }
            testRealSpool(printer);
        }
    }

    std::printf("\n==== 断言 %d 项，失败 %d 项 ====\n", g_checkCount, g_failCount);
    return g_failCount == 0 ? 0 : 1;
}
