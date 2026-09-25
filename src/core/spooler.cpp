// =============================================================================
// 文件名：src/core/spooler.cpp
// 作用  ：Windows 打印后台访问层的实现（winspool API）。
// 关键点：1) 权限：attach() 先申请 PRINTER_ALL_ACCESS（能暂停/取消作业），
//            普通权限下会失败，此时退回默认只读权限，保证“统计与收费”仍可用；
//         2) 句柄 RAII：attach 打开、detach/析构关闭，异常路径也不会漏句柄；
//         3) EnumJobs 用“缓冲区不足则扩大重试”的标准写法；
//         4) 页数取值：TotalPages 是驱动上报总页数，PagesPrinted 是已打印页数；
//            某些驱动只填其中一个，所以取两者较大值；都为 0 时由界面让老板手工填。
//         5) 份数：JOB_INFO_2 没有直接给份数，从 pDevMode->dmCopies 取。
// =============================================================================

#include "core/spooler.h"

#ifndef NOMINMAX
#define NOMINMAX   // 禁止 windows.h 定义 min/max 宏，否则与 std::min/std::max 冲突
#endif
#include <windows.h>
#include <winspool.h>   // OpenPrinter / EnumJobs / SetPrinter / SetJob / GetPrinter

#include <vector>

namespace printpay {

// JOB_STATUS_PRINTED 定义在 winspool.h 里，这里实现 print_job.h 声明的辅助函数
bool PrintJobInfo::looksPrinted() const
{
    return (windowsStatus & JOB_STATUS_PRINTED) != 0;
}

namespace {

// 把 Windows 的 SYSTEMTIME 转成毫秒时间戳（作业提交时间用）
qint64 systemTimeToMs(const SYSTEMTIME& time)
{
    SYSTEMTIME local = time;
    FILETIME fileTime;
    if (!SystemTimeToFileTime(&local, &fileTime)) {
        return 0;
    }
    ULARGE_INTEGER value;
    value.LowPart = fileTime.dwLowDateTime;
    value.HighPart = fileTime.dwHighDateTime;
    // FILETIME 是 100 纳秒单位、起点 1601-01-01；换算成 Unix 毫秒时间戳
    return static_cast<qint64>(value.QuadPart / 10000ULL) - 11644473600000LL;
}

// 从 DEVMODE 里取份数，取值异常时返回 1
int copiesFromDevMode(const DEVMODE* devMode)
{
    if (devMode == nullptr) {
        return 1;
    }
    const WORD copies = devMode->dmCopies;
    return copies > 0 ? static_cast<int>(copies) : 1;
}

const char16_t* wideName(const QString& text)
{
    return reinterpret_cast<const char16_t*>(text.utf16());
}

}  // namespace

Spooler::Spooler() = default;

Spooler::~Spooler()
{
    detach();
}

QString Spooler::errorText(quint32 code)
{
    if (code == 0) {
        return QStringLiteral("（无错误信息）");
    }
    // 用 FormatMessage 把系统错误码翻译成中文说明（找不到说明时退化为错误码）
    LPWSTR buffer = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
        LocalFree(buffer);
    }
    if (message.isEmpty()) {
        return QStringLiteral("错误码 %1").arg(code);
    }
    return QStringLiteral("错误码 %1：%2").arg(code).arg(message);
}

QString Spooler::printerStatusText(quint32 status)
{
    if (status == 0) {
        return QStringLiteral("就绪");
    }
    QStringList parts;
    if (status & PRINTER_STATUS_PAUSED)            parts << QStringLiteral("已暂停");
    if (status & PRINTER_STATUS_ERROR)             parts << QStringLiteral("故障");
    if (status & PRINTER_STATUS_PENDING_DELETION)  parts << QStringLiteral("正在删除");
    if (status & PRINTER_STATUS_PAPER_JAM)         parts << QStringLiteral("卡纸");
    if (status & PRINTER_STATUS_PAPER_OUT)         parts << QStringLiteral("缺纸");
    if (status & PRINTER_STATUS_MANUAL_FEED)       parts << QStringLiteral("需手动进纸");
    if (status & PRINTER_STATUS_PAPER_PROBLEM)     parts << QStringLiteral("纸张问题");
    if (status & PRINTER_STATUS_OFFLINE)           parts << QStringLiteral("脱机");
    if (status & PRINTER_STATUS_IO_ACTIVE)         parts << QStringLiteral("正在通信");
    if (status & PRINTER_STATUS_BUSY)              parts << QStringLiteral("忙");
    if (status & PRINTER_STATUS_PRINTING)          parts << QStringLiteral("正在打印");
    if (status & PRINTER_STATUS_OUTPUT_BIN_FULL)   parts << QStringLiteral("出纸槽已满");
    if (status & PRINTER_STATUS_NOT_AVAILABLE)     parts << QStringLiteral("不可用");
    if (status & PRINTER_STATUS_WAITING)           parts << QStringLiteral("等待中");
    if (status & PRINTER_STATUS_PROCESSING)        parts << QStringLiteral("处理中");
    if (status & PRINTER_STATUS_INITIALIZING)      parts << QStringLiteral("初始化");
    if (status & PRINTER_STATUS_WARMING_UP)        parts << QStringLiteral("预热中");
    if (status & PRINTER_STATUS_TONER_LOW)         parts << QStringLiteral("碳粉不足");
    if (status & PRINTER_STATUS_NO_TONER)          parts << QStringLiteral("无碳粉");
    if (status & PRINTER_STATUS_USER_INTERVENTION) parts << QStringLiteral("需人工处理");
    if (status & PRINTER_STATUS_OUT_OF_MEMORY)     parts << QStringLiteral("内存不足");
    if (status & PRINTER_STATUS_DOOR_OPEN)         parts << QStringLiteral("盖子未关");
    if (status & PRINTER_STATUS_SERVER_UNKNOWN)    parts << QStringLiteral("状态未知");
    if (status & PRINTER_STATUS_POWER_SAVE)        parts << QStringLiteral("省电模式");
    return parts.isEmpty() ? QStringLiteral("状态未知(%1)").arg(status)
                           : parts.join(QStringLiteral("、"));
}

QVector<PrinterInfo> Spooler::listPrinters(QString* error)
{
    QVector<PrinterInfo> result;

    DWORD needed = 0;
    DWORD returned = 0;
    // 第一次调用只为取所需字节数；返回 false 且 LastError 为 ERROR_INSUFFICIENT_BUFFER 属正常
    EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                  nullptr, 0, &needed, &returned);
    if (needed == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("未发现任何打印机（打印后台服务是否已启动？）");
        }
        return result;
    }

    std::vector<BYTE> buffer(needed);
    if (!EnumPrintersW(PRINTER_ENUM_LOCAL | PRINTER_ENUM_CONNECTIONS, nullptr, 2,
                       buffer.data(), needed, &needed, &returned)) {
        if (error != nullptr) {
            *error = QStringLiteral("枚举打印机失败：") + errorText(GetLastError());
        }
        return result;
    }

    const PRINTER_INFO_2W* printers = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
    for (DWORD i = 0; i < returned; ++i) {
        PrinterInfo info;
        info.name = printers[i].pPrinterName != nullptr
                        ? QString::fromWCharArray(printers[i].pPrinterName) : QString();
        info.driver = printers[i].pDriverName != nullptr
                          ? QString::fromWCharArray(printers[i].pDriverName) : QString();
        info.port = printers[i].pPortName != nullptr
                        ? QString::fromWCharArray(printers[i].pPortName) : QString();
        info.status = printers[i].Status;
        info.paused = (printers[i].Status & PRINTER_STATUS_PAUSED) != 0;
        info.isDefault = (printers[i].Attributes & PRINTER_ATTRIBUTE_DEFAULT) != 0;
        result.push_back(info);
    }
    return result;
}

bool Spooler::attach(const QString& printerName, QString* error)
{
    detach();
    if (printerName.isEmpty()) {
        if (error != nullptr) {
            *error = QStringLiteral("打印机名为空");
        }
        return false;
    }

    const wchar_t* name = reinterpret_cast<const wchar_t*>(printerName.utf16());

    // 第 1 步：申请完整权限（这样才有权暂停打印机、取消作业）
    PRINTER_DEFAULTSW defaults;
    defaults.pDatatype = nullptr;
    defaults.pDevMode = nullptr;
    defaults.DesiredAccess = PRINTER_ALL_ACCESS;

    HANDLE handle = nullptr;
    if (OpenPrinterW(const_cast<LPWSTR>(name), &handle, &defaults)) {
        handle_ = handle;
        name_ = printerName;
        canControl_ = true;
        return true;
    }

    const DWORD controlError = GetLastError();

    // 第 2 步：退回默认（只读）权限再试一次 —— 没有管理员权限时，至少保证
    // “统计页数 + 收款”这些核心功能可用，不能因为暂停功能可用不了就整个罢工
    if (OpenPrinterW(const_cast<LPWSTR>(name), &handle, nullptr)) {
        handle_ = handle;
        name_ = printerName;
        canControl_ = false;
        if (error != nullptr) {
            *error = QStringLiteral(
                "以只读权限打开了打印机（%1）。\n"
                "当前权限下无法暂停打印队列，也就是“打印前必须老板同意”会失效；\n"
                "请在程序里点“以管理员身份重启”，或右键程序选择“以管理员身份运行”。")
                         .arg(errorText(controlError));
        }
        return true;   // 注意：仍然返回 true，因为基本的监控能力是可用的
    }

    // 两次都失败：打印机不存在 / 打印服务未启动 / 名称写错
    if (error != nullptr) {
        *error = QStringLiteral("打开打印机“%1”失败：%2")
                     .arg(printerName, errorText(GetLastError()));
    }
    return false;
}

void Spooler::detach()
{
    if (handle_ != nullptr) {
        ClosePrinter(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
    name_.clear();
    canControl_ = false;
}

bool Spooler::isAttached() const
{
    return handle_ != nullptr;
}

QString Spooler::printerName() const
{
    return name_;
}

bool Spooler::canControl() const
{
    return canControl_;
}

bool Spooler::isPaused(QString* error) const
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("尚未绑定打印机");
        }
        return false;
    }
    DWORD needed = 0;
    GetPrinterW(static_cast<HANDLE>(handle_), 2, nullptr, 0, &needed);
    if (needed == 0) {
        if (error != nullptr) {
            *error = QStringLiteral("读取打印机状态失败：") + errorText(GetLastError());
        }
        return false;
    }
    std::vector<BYTE> buffer(needed);
    if (!GetPrinterW(static_cast<HANDLE>(handle_), 2, buffer.data(), needed, &needed)) {
        if (error != nullptr) {
            *error = QStringLiteral("读取打印机状态失败：") + errorText(GetLastError());
        }
        return false;
    }
    const PRINTER_INFO_2W* info = reinterpret_cast<const PRINTER_INFO_2W*>(buffer.data());
    return (info->Status & PRINTER_STATUS_PAUSED) != 0;
}

bool Spooler::pausePrinter(QString* error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("尚未绑定打印机");
        }
        return false;
    }
    if (!canControl_) {
        if (error != nullptr) {
            *error = QStringLiteral("当前没有管理员权限，无法暂停打印机队列（请以管理员身份重启程序）");
        }
        return false;
    }
    // SetPrinter 的第二参数为 0 表示“控制命令在第四个参数里”
    if (!SetPrinterW(static_cast<HANDLE>(handle_), 0, nullptr, PRINTER_CONTROL_PAUSE)) {
        if (error != nullptr) {
            *error = QStringLiteral("暂停打印机失败：") + errorText(GetLastError());
        }
        return false;
    }
    return true;
}

bool Spooler::resumePrinter(QString* error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("尚未绑定打印机");
        }
        return false;
    }
    if (!canControl_) {
        if (error != nullptr) {
            *error = QStringLiteral("当前没有管理员权限，无法恢复打印机队列（请以管理员身份重启程序）");
        }
        return false;
    }
    if (!SetPrinterW(static_cast<HANDLE>(handle_), 0, nullptr, PRINTER_CONTROL_RESUME)) {
        if (error != nullptr) {
            *error = QStringLiteral("恢复打印机失败：") + errorText(GetLastError());
        }
        return false;
    }
    return true;
}

bool Spooler::cancelJob(quint32 jobId, QString* error)
{
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("尚未绑定打印机");
        }
        return false;
    }
    if (!canControl_) {
        if (error != nullptr) {
            *error = QStringLiteral("当前没有管理员权限，无法取消打印作业（请以管理员身份重启程序）");
        }
        return false;
    }
    if (!SetJobW(static_cast<HANDLE>(handle_), jobId, 0, nullptr, JOB_CONTROL_CANCEL)) {
        if (error != nullptr) {
            *error = QStringLiteral("取消作业 %1 失败：%2").arg(jobId).arg(errorText(GetLastError()));
        }
        return false;
    }
    return true;
}

QVector<PrintJobInfo> Spooler::enumerateJobs(QString* error) const
{
    QVector<PrintJobInfo> result;
    if (handle_ == nullptr) {
        if (error != nullptr) {
            *error = QStringLiteral("尚未绑定打印机");
        }
        return result;
    }

    HANDLE handle = static_cast<HANDLE>(handle_);
    DWORD needed = 0;
    DWORD returned = 0;
    // 先从第 0 个作业取最多 99 个；缓冲区不够时系统会把所需大小写进 needed
    EnumJobsW(handle, 0, 99, 2, nullptr, 0, &needed, &returned);
    if (needed == 0) {
        return result;   // 队列为空（最常见的情况，不是错误）
    }

    std::vector<BYTE> buffer(needed);
    if (!EnumJobsW(handle, 0, 99, 2, buffer.data(), needed, &needed, &returned)) {
        if (error != nullptr) {
            *error = QStringLiteral("读取打印队列失败：") + errorText(GetLastError());
        }
        return result;
    }

    const JOB_INFO_2W* jobs = reinterpret_cast<const JOB_INFO_2W*>(buffer.data());
    for (DWORD i = 0; i < returned; ++i) {
        const JOB_INFO_2W& job = jobs[i];
        PrintJobInfo info;
        info.jobId = job.JobId;
        info.printerName = job.pPrinterName != nullptr
                               ? QString::fromWCharArray(job.pPrinterName) : name_;
        info.document = job.pDocument != nullptr ? QString::fromWCharArray(job.pDocument) : QString();
        info.userName = job.pUserName != nullptr ? QString::fromWCharArray(job.pUserName) : QString();
        info.machineName = job.pMachineName != nullptr
                               ? QString::fromWCharArray(job.pMachineName) : QString();
        info.statusText = job.pStatus != nullptr ? QString::fromWCharArray(job.pStatus) : QString();
        info.windowsStatus = job.Status;
        info.totalPages = static_cast<int>(job.TotalPages);
        info.printedPages = static_cast<int>(job.PagesPrinted);
        info.copies = copiesFromDevMode(job.pDevMode);
        info.submittedMs = systemTimeToMs(job.Submitted);
        result.push_back(info);
    }
    return result;
}

}  // namespace printpay
