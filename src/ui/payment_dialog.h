// =============================================================================
// 文件名：src/ui/payment_dialog.h
// 作用  ：收款弹窗。打印完成后弹出，向客户展示微信/支付宝收款码、应付金额和微信号。
// 位置  ：由 MainWindow 在收到 JobFlow::paymentRequested 信号时弹出（模态）。
// 注意  ：1) 窗口置顶（WindowStaysOnTopHint），避免被客户的打印预览窗口挡住；
//         2) 页数可手工修正：有些打印机驱动不上报页数（TotalPages=0），
//            这时让老板按实际页数填，金额实时重算 —— 不靠猜，绝不算错钱；
//         3) Esc / 关闭窗口 = 记为“未收款”，流水不会丢，会体现在今日“未收金额”里。
// =============================================================================
#pragma once

#include <QDialog>

#include "core/print_job.h"
#include "core/settings.h"

class QLabel;
class QSpinBox;
class QPushButton;

namespace printpay {

class PaymentDialog : public QDialog {
    Q_OBJECT

public:
    // 参数 job：完成的打印作业；amountCents：状态机算出的金额（页数未知时为 0）；
    //       settings：读取微信号、单价与显示选项。
    PaymentDialog(const PrintJobInfo& job, int amountCents,
                  const AppSettings& settings, QWidget* parent = nullptr);

    // 老板确认后的页数（可能被手工修正过）
    int pages() const;
    // 是否已收款（点“已收款”按钮为 true；关闭/Esc/稍后处理为 false）
    bool paid() const;

private slots:
    void onPagesChanged();

private:
    // 组装收款码区域：加载图片并按屏幕高度缩放；找不到图片时给出明确提示
    QWidget* createQrArea();
    void updateAmountLabel();
    // 载入一张收款码图片（失败返回空 pixmap）
    QPixmap loadQr(const QString& fileName) const;

    PrintJobInfo job_;
    AppSettings settings_;
    QSpinBox* pagesSpin_ = nullptr;
    QLabel* amountLabel_ = nullptr;
    QLabel* pagesHint_ = nullptr;
    QLabel* qrHint_ = nullptr;
    bool paid_ = false;
};

}  // namespace printpay
