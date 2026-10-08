// =============================================================================
// 文件名：src/ui/settings_dialog.cpp
// 作用  ：设置窗口实现（打印机、计费、策略、常驻方式，以及“查看历史打印总量”）。
// =============================================================================
#include "ui/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

#include "core/billing.h"
#include "core/spooler.h"

namespace printpay {
namespace {

// 把毫秒时间戳格式化成可读时间；0 表示没有记录
QString formatTime(qint64 ms)
{
    if (ms <= 0) {
        return QStringLiteral("—");
    }
    return QDateTime::fromMSecsSinceEpoch(ms).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
}

}  // namespace

SettingsDialog::SettingsDialog(const AppSettings& settings, Store* store, QWidget* parent)
    : QDialog(parent)
    , settings_(settings)
    , store_(store)
{
    setWindowTitle(QStringLiteral("设置 · 打印计费助手"));
    setMinimumWidth(560);

    QVBoxLayout* root = new QVBoxLayout(this);

    // ---------------- 打印监控 ----------------
    QGroupBox* printBox = new QGroupBox(QStringLiteral("打印监控"), this);
    QFormLayout* printForm = new QFormLayout(printBox);

    QHBoxLayout* printerRow = new QHBoxLayout;
    printerBox_ = new QComboBox(printBox);
    printerBox_->setMinimumWidth(320);
    QPushButton* refreshButton = new QPushButton(QStringLiteral("刷新"), printBox);
    printerRow->addWidget(printerBox_, 1);
    printerRow->addWidget(refreshButton);
    printForm->addRow(QStringLiteral("要监控的打印机："), printerRow);

    intervalSpin_ = new QSpinBox(printBox);
    intervalSpin_->setRange(200, 10000);
    intervalSpin_->setSingleStep(100);
    intervalSpin_->setSuffix(QStringLiteral(" 毫秒"));
    intervalSpin_->setValue(settings_.pollIntervalMs);
    printForm->addRow(QStringLiteral("队列检查间隔："), intervalSpin_);
    root->addWidget(printBox);

    // ---------------- 计费 ----------------
    QGroupBox* billingBox = new QGroupBox(QStringLiteral("计费"), this);
    QFormLayout* billingForm = new QFormLayout(billingBox);

    priceSpin_ = new QDoubleSpinBox(billingBox);
    priceSpin_->setRange(0.0, 1000.0);
    priceSpin_->setDecimals(2);
    priceSpin_->setSingleStep(0.05);
    priceSpin_->setSuffix(QStringLiteral(" 元/页"));
    priceSpin_->setValue(settings_.pricePerPageCents / 100.0);
    billingForm->addRow(QStringLiteral("每页单价："), priceSpin_);

    minChargeSpin_ = new QDoubleSpinBox(billingBox);
    minChargeSpin_->setRange(0.0, 10000.0);
    minChargeSpin_->setDecimals(2);
    minChargeSpin_->setSingleStep(0.10);
    minChargeSpin_->setSuffix(QStringLiteral(" 元"));
    minChargeSpin_->setValue(settings_.minChargeCents / 100.0);
    billingForm->addRow(QStringLiteral("最低消费："), minChargeSpin_);

    wechatEdit_ = new QLineEdit(billingBox);
    wechatEdit_->setText(settings_.wechatId);
    wechatEdit_->setPlaceholderText(QStringLiteral("客户加你时用的微信号"));
    billingForm->addRow(QStringLiteral("微信号："), wechatEdit_);
    root->addWidget(billingBox);

    // ---------------- 策略 ----------------
    QGroupBox* policyBox = new QGroupBox(QStringLiteral("收费与审批策略"), this);
    QVBoxLayout* policyLayout = new QVBoxLayout(policyBox);

    chargeOthersBox_ = new QCheckBox(QStringLiteral("其他人（含未登录）打印要收费"), policyBox);
    chargeOthersBox_->setChecked(settings_.chargeOthers);
    chargeAdminBox_ = new QCheckBox(
        QStringLiteral("管理员登录后打印免费（不勾选则管理员也收费）"), policyBox);
    chargeAdminBox_->setChecked(!settings_.chargeAdmin);

    approveOthersBox_ = new QCheckBox(
        QStringLiteral("其他人打印前需要我同意（会先暂停打印机队列，等你点“同意”）"), policyBox);
    approveOthersBox_->setChecked(settings_.requireApprovalForOthers);
    approveAdminBox_ = new QCheckBox(QStringLiteral("我自己打印也需要同意"), policyBox);
    approveAdminBox_->setChecked(settings_.requireApprovalForAdmin);
    autoPaymentBox_ = new QCheckBox(QStringLiteral("打印完成后自动弹出收款码"), policyBox);
    autoPaymentBox_->setChecked(settings_.autoPaymentDialog);
    fullScreenBox_ = new QCheckBox(QStringLiteral("收款码窗口最大化显示（屏幕大、客户好扫）"), policyBox);
    fullScreenBox_->setChecked(settings_.fullScreenPayment);

    policyLayout->addWidget(chargeOthersBox_);
    policyLayout->addWidget(chargeAdminBox_);
    policyLayout->addWidget(approveOthersBox_);
    policyLayout->addWidget(approveAdminBox_);
    policyLayout->addWidget(autoPaymentBox_);
    policyLayout->addWidget(fullScreenBox_);

    // ---------------- 常驻方式 ----------------
    closeToTrayBox_ = new QCheckBox(
        QStringLiteral("点关闭按钮时最小化到任务栏托盘（程序继续在后台监控打印）"), policyBox);
    closeToTrayBox_->setChecked(settings_.closeToTray);
    startMinimizedBox_ = new QCheckBox(
        QStringLiteral("启动时不显示主窗口，只留托盘图标（适合开机自启）"), policyBox);
    startMinimizedBox_->setChecked(settings_.startMinimized);
    policyLayout->addWidget(closeToTrayBox_);
    policyLayout->addWidget(startMinimizedBox_);
    root->addWidget(policyBox);

    // ---------------- 打印量统计 ----------------
    // 只查询、不改配置：老板想知道“从开始用到现在一共打了多少”时点这里
    QGroupBox* statsBox = new QGroupBox(QStringLiteral("打印量统计"), this);
    QHBoxLayout* statsLayout = new QHBoxLayout(statsBox);
    QLabel* statsHint = new QLabel(
        QStringLiteral("查看从开始使用到现在的累计打印量：多少单、多少份、多少页，以及累计收款。"), statsBox);
    statsHint->setWordWrap(true);
    QPushButton* statsButton = new QPushButton(QStringLiteral("查看历史打印总量"), statsBox);
    statsLayout->addWidget(statsHint, 1);
    statsLayout->addWidget(statsButton);
    root->addWidget(statsBox);

    hintLabel_ = new QLabel(this);
    hintLabel_->setStyleSheet(QStringLiteral("color:#c81e1e;"));
    hintLabel_->setWordWrap(true);
    root->addWidget(hintLabel_);

    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* cancelButton = new QPushButton(QStringLiteral("取消"), this);
    QPushButton* okButton = new QPushButton(QStringLiteral("确定"), this);
    okButton->setDefault(true);
    buttons->addStretch(1);
    buttons->addWidget(cancelButton);
    buttons->addWidget(okButton);
    root->addLayout(buttons);

    connect(statsButton, &QPushButton::clicked, this, &SettingsDialog::showTotalStats);
    connect(refreshButton, &QPushButton::clicked, this, &SettingsDialog::refreshPrinters);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(okButton, &QPushButton::clicked, this, &SettingsDialog::onAccept);

    refreshPrinters();
}

void SettingsDialog::showTotalStats()
{
    if (store_ == nullptr || !store_->isOpen()) {
        QMessageBox::warning(this, QStringLiteral("打印计费助手"),
                             QStringLiteral("数据库不可用，无法统计"));
        return;
    }

    QString error;
    const TotalSummary total = store_->totalSummary(&error);
    if (!error.isEmpty()) {
        QMessageBox::warning(this, QStringLiteral("打印计费助手"), error);
        return;
    }
    if (total.jobs <= 0) {
        QMessageBox::information(this, QStringLiteral("历史打印总量"),
                                 QStringLiteral("还没有任何打印记录。\n\n"
                                                "等程序监控到第一次打印后，这里就会显示累计数据。"));
        return;
    }

    const int unpaid = total.amountCents - total.paidCents;
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("历史打印总量"));
    box.setIcon(QMessageBox::Information);
    // 主标题：一眼看到“多少单、多少份、多少页”
    box.setText(QStringLiteral("累计打印 %1 单，共 %2 份，共 %3 页")
                    .arg(total.jobs)
                    .arg(total.copies)
                    .arg(total.pages));
    // 副标题：金额与统计范围（从第一条记录到最后一条记录的时间）
    box.setInformativeText(
        QStringLiteral("累计应收 %1（已收 %2，未收 %3）\n"
                       "统计范围：%4 ～ %5\n\n"
                       "（按每页 %6 计费；如需清零重新统计，退出程序后删除 printpay.db 即可）")
            .arg(formatCents(total.amountCents))
            .arg(formatCents(total.paidCents))
            .arg(formatCents(unpaid))
            .arg(formatTime(total.firstMs))
            .arg(formatTime(total.lastMs))
            .arg(formatCents(settings_.pricePerPageCents)));
    box.exec();
}

void SettingsDialog::refreshPrinters()
{
    printerBox_->clear();
    QString error;
    const QVector<PrinterInfo> printers = Spooler::listPrinters(&error);
    for (const PrinterInfo& printer : printers) {
        printerBox_->addItem(QStringLiteral("%1（%2）").arg(printer.name, printer.port), printer.name);
    }
    if (printers.isEmpty()) {
        printerBox_->addItem(QStringLiteral("未发现打印机"), QString());
        hintLabel_->setText(error.isEmpty() ? QStringLiteral("未发现任何打印机") : error);
        return;
    }
    // 选中当前配置里的打印机；若已不在列表里，退回第一台并提示
    const int index = printerBox_->findData(settings_.printerName);
    if (index >= 0) {
        printerBox_->setCurrentIndex(index);
    } else if (!settings_.printerName.isEmpty()) {
        hintLabel_->setText(QStringLiteral("原先配置的打印机「%1」已不存在，请重新选择")
                                .arg(settings_.printerName));
    }
}

void SettingsDialog::onAccept()
{
    const QString printer = printerBox_->currentData().toString();
    if (printer.isEmpty()) {
        hintLabel_->setText(QStringLiteral("请先选择要监控的打印机"));
        return;
    }
    const QString wechat = wechatEdit_->text().trimmed();
    if (wechat.isEmpty()) {
        hintLabel_->setText(QStringLiteral("请填写微信号，客户需要加你微信才能打印"));
        return;
    }

    settings_.printerName = printer;
    settings_.pollIntervalMs = intervalSpin_->value();
    // 元 -> 分：用四舍五入消除浮点表示误差（0.1 元 * 100 = 10.000000000000002）
    settings_.pricePerPageCents = static_cast<int>(std::lround(priceSpin_->value() * 100.0));
    settings_.minChargeCents = static_cast<int>(std::lround(minChargeSpin_->value() * 100.0));
    settings_.wechatId = wechat;
    settings_.chargeOthers = chargeOthersBox_->isChecked();
    settings_.chargeAdmin = !chargeAdminBox_->isChecked();   // 复选框是“免费”，取反得到“收费”
    settings_.requireApprovalForOthers = approveOthersBox_->isChecked();
    settings_.requireApprovalForAdmin = approveAdminBox_->isChecked();
    settings_.autoPaymentDialog = autoPaymentBox_->isChecked();
    settings_.fullScreenPayment = fullScreenBox_->isChecked();
    settings_.closeToTray = closeToTrayBox_->isChecked();
    settings_.startMinimized = startMinimizedBox_->isChecked();

    accept();
}

}  // namespace printpay
