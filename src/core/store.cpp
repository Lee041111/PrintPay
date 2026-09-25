// =============================================================================
// 文件名：src/core/store.cpp
// 作用  ：SQLite 存储层实现（账号 + 打印流水）。
// 注意  ：1) 建表语句都带 IF NOT EXISTS，可重复执行；
//         2) 时间统一用毫秒时间戳（INTEGER），与界面显示解耦；
//         3) 口令用 SHA-256（用户名作盐）存哈希，登录时比对哈希；
//         4) 今日汇总用 SQL 聚合，避免把几万行读进内存。
// =============================================================================
#include "core/store.h"

#include <QCryptographicHash>
#include <QDate>
#include <QDateTime>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

namespace printpay {
namespace {

void setError(QString* error, const QString& message)
{
    if (error != nullptr) {
        *error = message;
    }
}

const char* kRoleAdmin = "admin";
const char* kRoleUser = "user";

QString roleToString(AccountRole role)
{
    return QString::fromLatin1(role == AccountRole::Admin ? kRoleAdmin : kRoleUser);
}

// 今天 00:00 的毫秒时间戳，用于“今日汇总”
qint64 todayStartMs()
{
    const QDateTime start(QDate::currentDate(), QTime(0, 0, 0));
    return start.toMSecsSinceEpoch();
}

}  // namespace

Store::~Store()
{
    close();
}

bool Store::open(const QString& filePath, QString* error)
{
    if (open_) {
        return true;
    }
    if (!QSqlDatabase::isDriverAvailable(QStringLiteral("QSQLITE"))) {
        setError(error, QStringLiteral("缺少 QSQLITE 驱动（Qt 安装不完整）"));
        return false;
    }
    if (!QSqlDatabase::contains(connectionName_)) {
        QSqlDatabase database = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
        database.setDatabaseName(filePath);
    }
    QSqlDatabase database = QSqlDatabase::database(connectionName_);
    if (!database.open()) {
        setError(error, QStringLiteral("打开数据库失败：") + database.lastError().text());
        return false;
    }
    open_ = true;
    return true;
}

void Store::close()
{
    if (QSqlDatabase::contains(connectionName_)) {
        {
            QSqlDatabase database = QSqlDatabase::database(connectionName_);
            if (database.isOpen()) {
                database.close();
            }
        }
        // 先让局部变量析构再移除连接，避免 “connection is still in use” 警告
        QSqlDatabase::removeDatabase(connectionName_);
    }
    open_ = false;
}

bool Store::isOpen() const
{
    return open_;
}

QString Store::hashPassword(const QString& userName, const QString& password)
{
    // 用户名作盐：即使两个人用同一个弱口令，库里的哈希也不一样
    const QByteArray raw = (QStringLiteral("printpay|") + userName + QLatin1Char('|') + password)
                               .toUtf8();
    return QString::fromLatin1(QCryptographicHash::hash(raw, QCryptographicHash::Sha256).toHex());
}

bool Store::ensureSchema(QString* createdPassword, QString* error)
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return false;
    }
    QSqlDatabase database = QSqlDatabase::database(connectionName_);
    QSqlQuery query(database);

    const QStringList statements = {
        QStringLiteral("CREATE TABLE IF NOT EXISTS account ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "user_name TEXT NOT NULL UNIQUE,"
                       "password_hash TEXT NOT NULL,"
                       "role TEXT NOT NULL DEFAULT 'user',"
                       "created_at INTEGER NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS print_record ("
                       "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                       "ts INTEGER NOT NULL,"
                       "printer TEXT,"
                       "document TEXT,"
                       "owner TEXT,"
                       "account TEXT,"
                       "pages INTEGER,"
                       "copies INTEGER,"
                       "amount_cents INTEGER,"
                       "paid INTEGER DEFAULT 0,"
                       "free_of_charge INTEGER DEFAULT 0)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_record_ts ON print_record(ts)"),
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_record_paid ON print_record(paid)"),
    };
    for (const QString& statement : statements) {
        if (!query.exec(statement)) {
            setError(error, QStringLiteral("建表失败：") + query.lastError().text());
            return false;
        }
    }

    // 首次运行创建默认管理员：admin / admin123
    query.prepare(QStringLiteral("SELECT COUNT(*) FROM account"));
    if (!query.exec() || !query.next()) {
        setError(error, QStringLiteral("读取账号表失败：") + query.lastError().text());
        return false;
    }
    if (query.value(0).toInt() == 0) {
        const QString defaultPassword = QStringLiteral("admin123");
        QString addError;
        if (!addAccount(QStringLiteral("admin"), defaultPassword, AccountRole::Admin, &addError)) {
            setError(error, addError);
            return false;
        }
        if (createdPassword != nullptr) {
            *createdPassword = defaultPassword;
        }
    }
    return true;
}

bool Store::authenticate(const QString& userName, const QString& password,
                         AccountRole* role, QString* error) const
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("SELECT password_hash, role FROM account WHERE user_name = :name"));
    query.bindValue(QStringLiteral(":name"), userName);
    if (!query.exec()) {
        setError(error, QStringLiteral("查询账号失败：") + query.lastError().text());
        return false;
    }
    if (!query.next()) {
        setError(error, QStringLiteral("用户名或密码错误"));   // 不区分具体原因，避免暴露账号是否存在
        return false;
    }
    if (query.value(0).toString() != hashPassword(userName, password)) {
        setError(error, QStringLiteral("用户名或密码错误"));
        return false;
    }
    if (role != nullptr) {
        *role = query.value(1).toString() == QString::fromLatin1(kRoleAdmin)
                    ? AccountRole::Admin : AccountRole::User;
    }
    return true;
}

bool Store::addAccount(const QString& userName, const QString& password,
                       AccountRole role, QString* error)
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return false;
    }
    if (userName.trimmed().isEmpty() || password.isEmpty()) {
        setError(error, QStringLiteral("用户名和密码都不能为空"));
        return false;
    }
    if (password.size() < 6) {
        setError(error, QStringLiteral("密码至少 6 位"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral(
        "INSERT INTO account (user_name, password_hash, role, created_at) "
        "VALUES (:name, :hash, :role, :created)"));
    query.bindValue(QStringLiteral(":name"), userName.trimmed());
    query.bindValue(QStringLiteral(":hash"), hashPassword(userName.trimmed(), password));
    query.bindValue(QStringLiteral(":role"), roleToString(role));
    query.bindValue(QStringLiteral(":created"), QDateTime::currentMSecsSinceEpoch());
    if (!query.exec()) {
        setError(error, QStringLiteral("创建账号失败（用户名可能已存在）：") + query.lastError().text());
        return false;
    }
    return true;
}

bool Store::changePassword(const QString& userName, const QString& newPassword, QString* error)
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return false;
    }
    if (newPassword.size() < 6) {
        setError(error, QStringLiteral("新密码至少 6 位"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("UPDATE account SET password_hash = :hash WHERE user_name = :name"));
    query.bindValue(QStringLiteral(":hash"), hashPassword(userName, newPassword));
    query.bindValue(QStringLiteral(":name"), userName);
    if (!query.exec()) {
        setError(error, QStringLiteral("修改密码失败：") + query.lastError().text());
        return false;
    }
    if (query.numRowsAffected() <= 0) {
        setError(error, QStringLiteral("账号不存在：%1").arg(userName));
        return false;
    }
    return true;
}

int Store::accountCount(QString* error) const
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return 0;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    if (!query.exec(QStringLiteral("SELECT COUNT(*) FROM account")) || !query.next()) {
        setError(error, QStringLiteral("统计账号失败：") + query.lastError().text());
        return 0;
    }
    return query.value(0).toInt();
}

bool Store::addRecord(const PrintRecord& record, QString* error)
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral(
        "INSERT INTO print_record (ts, printer, document, owner, account, pages, copies, "
        "amount_cents, paid, free_of_charge) "
        "VALUES (:ts, :printer, :document, :owner, :account, :pages, :copies, :amount, :paid, :free)"));
    query.bindValue(QStringLiteral(":ts"), record.tsMs > 0 ? record.tsMs
                                                          : QDateTime::currentMSecsSinceEpoch());
    query.bindValue(QStringLiteral(":printer"), record.printerName);
    query.bindValue(QStringLiteral(":document"), record.document);
    query.bindValue(QStringLiteral(":owner"), record.owner);
    query.bindValue(QStringLiteral(":account"), record.account);
    query.bindValue(QStringLiteral(":pages"), record.pages);
    query.bindValue(QStringLiteral(":copies"), record.copies);
    query.bindValue(QStringLiteral(":amount"), record.amountCents);
    query.bindValue(QStringLiteral(":paid"), record.paid ? 1 : 0);
    query.bindValue(QStringLiteral(":free"), record.freeOfCharge ? 1 : 0);
    if (!query.exec()) {
        setError(error, QStringLiteral("写入流水失败：") + query.lastError().text());
        return false;
    }
    return true;
}

bool Store::markPaid(qint64 recordId, bool paid, QString* error)
{
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return false;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral("UPDATE print_record SET paid = :paid WHERE id = :id"));
    query.bindValue(QStringLiteral(":paid"), paid ? 1 : 0);
    query.bindValue(QStringLiteral(":id"), recordId);
    if (!query.exec()) {
        setError(error, QStringLiteral("更新收款状态失败：") + query.lastError().text());
        return false;
    }
    return true;
}

TodaySummary Store::todaySummary(QString* error) const
{
    TodaySummary summary;
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return summary;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    // 一次聚合出所有需要的数字，避免多次查库
    query.prepare(QStringLiteral(
        "SELECT COUNT(*), IFNULL(SUM(pages), 0), IFNULL(SUM(amount_cents), 0), "
        "IFNULL(SUM(CASE WHEN paid = 1 THEN amount_cents ELSE 0 END), 0) "
        "FROM print_record WHERE ts >= :start"));
    query.bindValue(QStringLiteral(":start"), todayStartMs());
    if (!query.exec() || !query.next()) {
        setError(error, QStringLiteral("统计今日流水失败：") + query.lastError().text());
        return summary;
    }
    summary.jobs = query.value(0).toInt();
    summary.pages = query.value(1).toInt();
    summary.amountCents = query.value(2).toInt();
    summary.paidCents = query.value(3).toInt();
    summary.unpaidCents = summary.amountCents - summary.paidCents;
    return summary;
}

QVector<PrintRecord> Store::recentRecords(int limit, QString* error) const
{
    QVector<PrintRecord> records;
    if (!open_) {
        setError(error, QStringLiteral("数据库尚未打开"));
        return records;
    }
    if (limit <= 0) {
        limit = 50;
    }
    QSqlQuery query(QSqlDatabase::database(connectionName_));
    query.prepare(QStringLiteral(
        "SELECT ts, printer, document, owner, account, pages, copies, amount_cents, paid, "
        "free_of_charge FROM print_record ORDER BY ts DESC LIMIT :limit"));
    query.bindValue(QStringLiteral(":limit"), limit);
    if (!query.exec()) {
        setError(error, QStringLiteral("读取流水失败：") + query.lastError().text());
        return records;
    }
    while (query.next()) {
        PrintRecord record;
        record.tsMs = query.value(0).toLongLong();
        record.printerName = query.value(1).toString();
        record.document = query.value(2).toString();
        record.owner = query.value(3).toString();
        record.account = query.value(4).toString();
        record.pages = query.value(5).toInt();
        record.copies = query.value(6).toInt();
        record.amountCents = query.value(7).toInt();
        record.paid = query.value(8).toInt() != 0;
        record.freeOfCharge = query.value(9).toInt() != 0;
        records.push_back(record);
    }
    return records;
}

}  // namespace printpay
