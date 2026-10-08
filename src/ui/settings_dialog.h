// =============================================================================
// 文件名：src/ui/settings_dialog.h
// 作用  ：设置窗口：选打印机、调单价与最低消费、填微信号、开关收费/审批策略，
//         以及查看“历史打印总量”。
// 位置  ：由 MainWindow 的“设置”按钮打开；确认后由 MainWindow 保存并应用到 JobFlow。
// 注意  ：1) 金额在界面上按“元”输入，内部统一换算成“分”存储（见 billing.h 的说明）；
//         2) 所有输入都做范围校验，保存前不会写入非法值；
//         3) “查看历史打印总量”按钮只做查询、不改配置，所以点它不会影响“确定”的结果。
// =============================================================================
#pragma once

#include <QDialog>

#include "core/settings.h"
#include "core/store.h"

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
    // 参数 settings：当前配置；store：已打开的数据库（用于查询历史打印总量，可为空）
    explicit SettingsDialog(const AppSettings& settings, Store* store,
                            QWidget* parent = nullptr);

    // 用户点“确定”后的配置（点取消时不应使用该结果）
    AppSettings result() const { return settings_; }

private slots:
    void refreshPrinters();
    void onAccept();
    // 弹窗显示全部历史的打印总量（多少单、多少份、多少页、累计金额）
    void showTotalStats();

private:
    AppSettings settings_;
    Store* store_ = nullptr;

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
    QCheckBox* closeToTrayBox_ = nullptr;
    QCheckBox* startMinimizedBox_ = nullptr;
    QLabel* hintLabel_ = nullptr;
};

}  // namespace printpay
