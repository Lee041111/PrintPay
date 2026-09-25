// =============================================================================
// 文件名：src/ui/approval_dialog.cpp
// 作用  ：审批弹窗实现。信息层级：先看“要打印什么、多少页”，再看“收多少钱”，
//         最后看“客户要加谁”——老板一眼就能决定。
// =============================================================================
#include "ui/approval_dialog.h"

#include <QDateTime>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "core/billing.h"

namespace printpay {

ApprovalDialog::ApprovalDialog(const PrintJobInfo& job, bool willCharge,
                               const AppSettings& settings, QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("新的打印作业 · 等待你的同意"));
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setWindowModality(Qt::ApplicationModal);
    setMinimumWidth(620);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setSpacing(12);

    QLabel* title = new QLabel(QStringLiteral("有客户要打印，请先确认"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    // 微信号：客户先加微信、得到同意后才能打印
    QLabel* wechat = new QLabel(
        QStringLiteral("请让客户先加微信：\n%1").arg(settings.wechatId), this);
    wechat->setAlignment(Qt::AlignCenter);
    QFont wechatFont = wechat->font();
    wechatFont.setPointSize(22);
    wechatFont.setBold(true);
    wechat->setFont(wechatFont);
    wechat->setStyleSheet(QStringLiteral("color:#117a37;background:#eaf7ee;padding:12px;border-radius:8px;"));
    root->addWidget(wechat);

    // 作业明细
    QGroupBox* box = new QGroupBox(QStringLiteral("打印明细"), this);
    QGridLayout* grid = new QGridLayout(box);
    const int pages = job.billablePages();
    const QString pagesText = pages > 0
                                  ? QStringLiteral("%1 页").arg(pages)
                                  : QStringLiteral("驱动未上报（在收款时手工填写）");

    grid->addWidget(new QLabel(QStringLiteral("文档："), box), 0, 0);
    QLabel* doc = new QLabel(job.document.isEmpty() ? QStringLiteral("（未提供文档名）") : job.document, box);
    doc->setWordWrap(true);
    grid->addWidget(doc, 0, 1);

    grid->addWidget(new QLabel(QStringLiteral("页数："), box), 1, 0);
    grid->addWidget(new QLabel(pagesText, box), 1, 1);

    grid->addWidget(new QLabel(QStringLiteral("份数："), box), 2, 0);
    grid->addWidget(new QLabel(QStringLiteral("%1 份").arg(job.copies), box), 2, 1);

    grid->addWidget(new QLabel(QStringLiteral("提交者："), box), 3, 0);
    grid->addWidget(new QLabel(job.userName.isEmpty() ? QStringLiteral("未知") : job.userName, box), 3, 1);

    grid->addWidget(new QLabel(QStringLiteral("打印机："), box), 4, 0);
    grid->addWidget(new QLabel(job.printerName, box), 4, 1);

    grid->addWidget(new QLabel(QStringLiteral("时间："), box), 5, 0);
    grid->addWidget(new QLabel(QDateTime::currentDateTime().toString(
                                   QStringLiteral("yyyy-MM-dd hh:mm:ss")), box), 5, 1);

    if (willCharge) {
        const int amount = computeAmountCents(pages, job.copies,
                                             settings.pricePerPageCents,
                                             settings.minChargeCents);
        grid->addWidget(new QLabel(QStringLiteral("预计收费："), box), 6, 0);
        QLabel* amountLabel = new QLabel(
            pages > 0 ? formatCents(amount) : QStringLiteral("待填页数后计算"), box);
        QFont amountFont = amountLabel->font();
        amountFont.setPointSize(14);
        amountFont.setBold(true);
        amountLabel->setFont(amountFont);
        amountLabel->setStyleSheet(QStringLiteral("color:#c81e1e;"));
        grid->addWidget(amountLabel, 6, 1);
    } else {
        grid->addWidget(new QLabel(QStringLiteral("计费："), box), 6, 0);
        grid->addWidget(new QLabel(QStringLiteral("本次免费（当前身份/策略为免费）"), box), 6, 1);
    }
    root->addWidget(box);

    // 三个决策按钮
    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* approve = new QPushButton(QStringLiteral("同意打印"), this);
    QPushButton* approveFree = new QPushButton(QStringLiteral("同意（本次免费）"), this);
    QPushButton* reject = new QPushButton(QStringLiteral("拒绝并取消打印"), this);
    approve->setMinimumHeight(48);
    approveFree->setMinimumHeight(48);
    reject->setMinimumHeight(48);
    QFont buttonFont = approve->font();
    buttonFont.setPointSize(12);
    approve->setFont(buttonFont);
    approveFree->setFont(buttonFont);
    reject->setFont(buttonFont);
    approve->setDefault(true);

    buttons->addWidget(reject);
    buttons->addStretch(1);
    buttons->addWidget(approveFree);
    buttons->addWidget(approve);
    root->addLayout(buttons);

    connect(approve, &QPushButton::clicked, this, [this] {
        decision_ = Decision::Approve;
        accept();
    });
    connect(approveFree, &QPushButton::clicked, this, [this] {
        decision_ = Decision::ApproveFree;
        accept();
    });
    connect(reject, &QPushButton::clicked, this, [this] {
        decision_ = Decision::Reject;
        accept();
    });
}

void ApprovalDialog::keyPressEvent(QKeyEvent* event)
{
    // Esc 不处理：强制老板点按钮明确表态，避免误关窗口导致作业被当作拒绝处理
    if (event->key() == Qt::Key_Escape) {
        return;
    }
    QDialog::keyPressEvent(event);
}

}  // namespace printpay
