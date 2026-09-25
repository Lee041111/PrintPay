// =============================================================================
// 文件名：src/ui/approval_dialog.h
// 作用  ：审批弹窗。客户提交打印后，弹出这个窗口让老板决定：
//           ① 同意打印（按配置收费）  ② 同意但本次免费  ③ 拒绝并取消打印
//         弹窗上会用大字显示微信号，提醒客户先加微信沟通。
// 位置  ：由 MainWindow 在收到 JobFlow::approvalRequested 信号时弹出（模态）。
// 注意  ：1) Esc 键被故意屏蔽 —— 老板必须明确选一个按钮，避免手滑把客户的作业取消；
//         2) 弹窗期间打印队列处于暂停状态（JobFlow 负责），所以客户的文件不会打出来。
// =============================================================================
#pragma once

#include <QDialog>

#include "core/print_job.h"
#include "core/settings.h"

class QKeyEvent;

namespace printpay {

class ApprovalDialog : public QDialog {
    Q_OBJECT

public:
    enum class Decision {
        Approve,      // 同意打印，正常收费
        ApproveFree,  // 同意打印，本次免费
        Reject        // 拒绝并取消打印
    };

    ApprovalDialog(const PrintJobInfo& job, bool willCharge,
                   const AppSettings& settings, QWidget* parent = nullptr);

    Decision decision() const { return decision_; }

protected:
    // 屏蔽 Esc 键：避免误触把客户作业取消掉
    void keyPressEvent(QKeyEvent* event) override;

private:
    Decision decision_ = Decision::Reject;
};

}  // namespace printpay
