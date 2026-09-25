// =============================================================================
// 文件名：src/core/spooler.h
// 作用  ：Windows 打印后台（Print Spooler）访问层。负责：
//           1) 枚举本机打印机；
//           2) 枚举某台打印机队列里的作业（含页数、份数、文档名、提交者）；
//           3) 暂停/恢复打印机（实现“客户先加微信、老板同意后再打印”）；
//           4) 删除指定作业（老板拒绝时取消打印）。
// 位置  ：整个程序与系统打交道的唯一入口，上层（JobFlow / 界面）不直接调用 Win32。
// 权限  ：非常关键的一点 —— “读队列”普通权限就够，但“暂停/恢复打印机、取消别人的作业”
//         需要管理员权限。因此 attach() 会先申请完整权限，失败就自动降级为只读，
//         并用 canControl() 告诉上层：能不能真正拦住打印。上层据此提示用户
//         “以管理员身份重启”（界面提供一键重启按钮），而不是默默失效。
// 注意  ：1) 本类不做线程调度，由上层用 QTimer 在主线程轮询（打印业务量小，
//            轮询比“变更通知 + 后台线程”简单得多，也不会出现线程安全问题）；
//         2) 句柄用 RAII 管理，析构自动 ClosePrinter，避免句柄泄漏；
//         3) 头文件刻意不包含 windows.h，句柄用 void* 保存，防止宏污染上层代码。
// =============================================================================
#pragma once

#include <QString>
#include <QVector>

#include "core/print_job.h"

namespace printpay {

// 一台打印机的概要信息
struct PrinterInfo {
    QString name;           // 打印机名（程序里用它作为唯一标识）
    QString driver;         // 驱动名
    QString port;           // 端口（USB004、PORTPROMPT: 等）
    quint32 status = 0;     // 状态位（PRINTER_STATUS_*）
    bool paused = false;    // 是否处于暂停状态
    bool isDefault = false; // 是否为默认打印机
};

// 打印后台访问类：一个实例对应一台打印机
class Spooler {
public:
    Spooler();
    ~Spooler();

    Spooler(const Spooler&) = delete;             // 持有系统句柄，禁止拷贝
    Spooler& operator=(const Spooler&) = delete;

    // 枚举本机所有打印机（含虚拟打印机）。失败时返回空列表并通过 error 给出原因。
    static QVector<PrinterInfo> listPrinters(QString* error = nullptr);

    // 把状态位翻译成中文描述（用于界面显示）。
    static QString printerStatusText(quint32 status);

    // 把 Windows 错误码翻译成“错误码 + 中文说明”。
    static QString errorText(quint32 code);

    // 绑定一台打印机：先按“完整权限”打开（能暂停/取消作业），失败则退回“只读权限”。
    // 返回 false 表示连只读都打不开（打印机不存在、打印服务未启动等）。
    // 打开后可用 canControl() 判断是否具备控制权限。
    bool attach(const QString& printerName, QString* error = nullptr);

    // 释放句柄（析构也会调用，可重复调用）。
    void detach();

    bool isAttached() const;
    QString printerName() const;

    // 是否具备“暂停/恢复打印机、取消任意作业”的权限（= 程序以管理员身份运行）。
    // 为 false 时程序仍能统计与收费，但无法真正拦住打印，需要提示用户重启提权。
    bool canControl() const;

    // 当前打印机是否处于暂停状态；未绑定时返回 false。
    bool isPaused(QString* error = nullptr) const;

    // 暂停 / 恢复打印机。无控制权限时返回 false，并在 error 里写清原因。
    bool pausePrinter(QString* error = nullptr);
    bool resumePrinter(QString* error = nullptr);

    // 取消队列里的一个作业（老板拒绝时使用）。
    bool cancelJob(quint32 jobId, QString* error = nullptr);

    // 枚举当前队列里的作业（JOB_INFO_2，含页数/份数/文档名）。
    QVector<PrintJobInfo> enumerateJobs(QString* error = nullptr) const;

private:
    void* handle_ = nullptr;   // 实际类型为 HANDLE，这里用 void* 避免在头文件引入 windows.h
    QString name_;             // 已绑定的打印机名
    bool canControl_ = false;  // 是否拿到了完整权限
};

}  // namespace printpay
