// =============================================================================
// 文件名：src/core/store.h
// 作用  ：SQLite 数据存储层，两张表：
//           account      账号表（用户名、口令哈希、角色）
//           print_record 打印流水表（每次打印的页数、金额、是否已收款）
// 位置  ：可执行文件同目录的 printpay.db。程序只在主线程访问它（本程序是单线程
//         GUI + 定时器模型），因此不需要额外加锁，也就不存在跨线程读写的坑。
// 安全  ：口令只存 SHA-256 摘要（加固定盐），不存明文；登录失败一律给同样的提示，
//         不区分“用户不存在”和“密码错误”。
// =============================================================================
#pragma once

#include <QString>
#include <QVector>

namespace printpay {

// 账号角色
enum class AccountRole {
    Admin,   // 管理员（老板本人）：默认不收费、不需要审批
    User     // 普通账号（店员/客户）：按配置收费并需要审批
};

// 一条打印流水
struct PrintRecord {
    qint64 tsMs = 0;            // 完成时间（毫秒时间戳）
    QString printerName;        // 打印机
    QString document;           // 文档名
    QString owner;              // Windows 用户名（作业提交者）
    QString account;            // 本程序里登录的账号（未登录时为空）
    int pages = 0;              // 页数（可能是老板手工修正后的值）
    int copies = 1;             // 份数
    int amountCents = 0;        // 应收金额（分）
    bool paid = false;          // 是否已收款
    bool freeOfCharge = false;  // 是否免费（管理员或策略配置为免费）
};

// 今日汇总（显示在界面顶部）
struct TodaySummary {
    int jobs = 0;             // 作业数
    int pages = 0;            // 总页数
    int amountCents = 0;      // 应收总额
    int paidCents = 0;        // 已收金额
    int unpaidCents = 0;      // 未收金额
};

class Store {
public:
    Store() = default;
    ~Store();

    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    // 打开数据库（连接名固定为 printpay，程序内只有这一条连接）。
    bool open(const QString& filePath, QString* error = nullptr);
    void close();
    bool isOpen() const;

    // 建表建索引 + 保证存在一个默认管理员账号（admin / admin123）。
    // 参数 createdPassword：首次创建时把默认口令回传，界面可据此提示用户尽快修改。
    bool ensureSchema(QString* createdPassword = nullptr, QString* error = nullptr);

    // ---- 账号 ----
    // 校验登录。成功返回 true 并回传角色；失败返回 false。
    bool authenticate(const QString& userName, const QString& password,
                      AccountRole* role, QString* error = nullptr) const;
    bool addAccount(const QString& userName, const QString& password,
                    AccountRole role, QString* error = nullptr);
    bool changePassword(const QString& userName, const QString& newPassword, QString* error = nullptr);
    int accountCount(QString* error = nullptr) const;
    static QString hashPassword(const QString& userName, const QString& password);

    // ---- 流水 ----
    bool addRecord(const PrintRecord& record, QString* error = nullptr);
    bool markPaid(qint64 recordId, bool paid, QString* error = nullptr);
    TodaySummary todaySummary(QString* error = nullptr) const;
    QVector<PrintRecord> recentRecords(int limit, QString* error = nullptr) const;

private:
    QString connectionName_ = QStringLiteral("printpay");
    bool open_ = false;
};

}  // namespace printpay
