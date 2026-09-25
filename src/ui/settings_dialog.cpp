// =============================================================================
// 文件名：src/ui/settings_dialog.cpp
// 作用  ：设置窗口实现。
// =============================================================================
#include "ui/settings_dialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <cmath>

#include "core/billing.h"
#include "core/spooler.h"

namespace printpay {

SettingsDialog::SettingsDialog(const AppSettings& settings, QWidget* parent)
    : QDialog(parent)
    , settings_(settings)
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
    chargeAdminBox_ = new QCheckBox(QStringLiteral("管理员（你自己）登录后打印免费"), policyBox);
    chargeAdminBox_->setChecked(!settings_.chargeAdmin);
    // 上面用的是反向措辞（“免费”），保存时再取反，避免复选框语义绕人
    chargeAdminBox_->setText(QStringLiteral("管理员登录后打印免费（不勾选则管理员也收费）"));

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

    connect(refreshButton, &QPushButton::clicked, this, &SettingsDialog::refreshPrinters);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    connect(okButton, &QPushButton::clicked, this, &SettingsDialog::onAccept);

    refreshPrinters();
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
