// =============================================================================
// 文件名：src/ui/settings_dialog.h
// 作用  ：设置窗口：选打印机、调单价与最低消费、填微信号、开关收费/审批策略。
// 位置  ：由 MainWindow 的“设置”按钮打开；确认后由 MainWindow 保存并应用到 JobFlow。
// 注意  ：金额在界面上按“元”输入，内部统一换算成“分”存储（见 billing.h 的说明）；
//         所有输入都做范围校验，保存前不会写入非法值。
// =============================================================================
#pragma once

#include <QDialog>

#include "core/settings.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QSpinBox;

namespace printpay {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(const AppSettings& settings, QWidget* parent = nullptr);

    // 用户点“确定”后的配置（点取消时不应使用该结果）
    AppSettings result() const { return settings_; }

private slots:
    void refreshPrinters();
    void onAccept();

private:
    AppSettings settings_;

    QComboBox* printerBox_ = nullptr;
    QSpinBox* intervalSpin_ = nullptr;
    QDoubleSpinBox* priceSpin_ = nullptr;
    QDoubleSpinBox* minChargeSpin_ = nullptr;
    QLineEdit* wechatEdit_ = nullptr;
    QCheckBox* chargeOthersBox_ = nullptr;
    QCheckBox* chargeAdminBox_ = nullptr;
    QCheckBox* approveOthersBox_ = nullptr;
    QCheckBox* approveAdminBox_ = nullptr;
    QCheckBox* autoPaymentBox_ = nullptr;
    QCheckBox* fullScreenBox_ = nullptr;
    QLabel* hintLabel_ = nullptr;
    QCheckBox* closeToTrayBox_ = nullptr;
    QCheckBox* startMinimizedBox_ = nullptr;
};

}  // namespace printpay
