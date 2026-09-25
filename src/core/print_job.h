// =============================================================================
// 文件名：src/core/print_job.h
// 作用  ：定义“一个打印作业”的数据结构，以及作业状态的枚举。
//         打印监控模块（spooler.cpp）把 Windows 打印队列里的原始信息翻译成它；
//         计费、审批、收款弹窗、流水记录都围绕它工作。
// 注意  ：字段全部为“填充后只读”的语义（结构体按值传递），不涉及线程共享。
// =============================================================================
#pragma once

#include <QDateTime>
#include <QString>

namespace printpay {

// 作业在系统里的流转状态（本程序自己维护，不代表 Windows 的位标志）
enum class JobState {
    Monitoring,   // 刚发现，正在排队/打印
    WaitingApprove, // 等待老板审批（此时打印队列已暂停）
    Printing,     // 已放行，正在打印
    Finished,     // 已从打印队列消失，等待收款或已收款
    Canceled      // 被拒绝/取消，需要删除队列中的作业
};

// 一个打印作业的完整信息
struct PrintJobInfo {
    quint32 jobId = 0;            // 打印队列里的作业号（唯一标识，用于去重与操作）
    QString printerName;          // 打印机名
    QString document;             // 文档名（客户打印的文件名）
    QString userName;             // 提交作业的 Windows 用户名
    QString machineName;          // 提交作业的计算机名（判断是否本机）
    QString statusText;           // Windows 给的作业状态文本（如“正在打印”）
    quint32 windowsStatus = 0;    // 作业状态位（JOB_STATUS_* 宏）
    int totalPages = 0;           // 驱动上报的总页数（某些驱动为 0）
    int printedPages = 0;         // 已打印页数（某些驱动不更新）
    int copies = 1;               // 份数（取自 DEVMODE，可能为 0/1）
    qint64 submittedMs = 0;       // 提交时间（毫秒时间戳）
    JobState state = JobState::Monitoring;  // 本程序维护的状态
    bool chargeable = false;      // 本次是否需要收费（老板可选“同意但免费”，会置为 false）
    bool approved = false;        // 是否已获批准（管理员或老板同意）
    bool paymentShown = false;    // 收款弹窗是否已经弹过（防止重复弹窗）
    // ---- 仅“模拟作业”使用（界面上点“模拟作业”测试流程时，不碰真实打印机）----
    bool simulated = false;              // 是否为模拟作业
    qint64 simulatedFinishMs = 0;        // 模拟作业的“打完时刻”（毫秒时间戳）

    // 计费页数：优先取“驱动上报总页数”，没有则用“已打印页数”，两者都为 0 时返回 0
    // （0 表示页数未知，收款弹窗里会允许老板手工修正）
    int billablePages() const
    {
        int pages = totalPages > printedPages ? totalPages : printedPages;
        return pages > 0 ? pages : 0;
    }

    // 是否已经打印完成：作业状态位里带 JOB_STATUS_PRINTED，或已从队列消失
    bool looksPrinted() const;
};

}  // namespace printpay
