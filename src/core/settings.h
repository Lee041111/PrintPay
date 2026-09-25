// =============================================================================
// 文件名：src/core/settings.h
// 作用  ：程序配置（config.ini）的读写。所有可调参数集中在这里：
//         打印机、轮询间隔、单价与最低消费、微信号、收费与审批策略、收款码路径等。
// 位置  ：可执行文件同目录的 config.ini，用 Qt 的 QSettings（INI 格式）读写，
//         好处是带注释、可直接用记事本改，也可以在界面“设置”里改。
// =============================================================================
#pragma once

#include <QString>

namespace printpay {

struct AppSettings {
    // ---- 打印监控 ----
    QString printerName;              // 要监控的打印机名（界面下拉框里选）
    int pollIntervalMs = 800;         // 队列轮询间隔（毫秒），越小越灵敏、越大越省 CPU

    // ---- 计费（金额都用“分”存，界面按“元”显示）----
    int pricePerPageCents = 10;       // 每页单价，默认 0.10 元
    int minChargeCents = 10;          // 最低消费，默认 0.10 元

    // ---- 联系方式 ----
    QString wechatId;                 // 微信号（收款弹窗与审批弹窗上显示）

    // ---- 策略开关 ----
    bool chargeOthers = true;              // 他人（含未登录）打印收费
    bool chargeAdmin = false;              // 管理员打印免费
    bool requireApprovalForOthers = true;  // 他人打印前需老板同意（会同 步暂停打印机队列）
    bool requireApprovalForAdmin = false;  // 管理员打印前是否需要同意（默认不需要）
    bool autoPaymentDialog = true;         // 打印完成后自动弹出收款码
    bool fullScreenPayment = false;        // 收款码全屏显示（屏幕大、客户好扫）

    // ---- 界面与常驻方式 ----
    bool closeToTray = true;      // 点窗口关闭按钮时：true = 最小化到托盘继续监控，false = 退出程序
    bool startMinimized = false;  // 启动时是否隐藏主窗口（只留托盘图标）；开机自启建议开

    // ---- 运行期状态（会写回配置文件，用于异常退出后恢复打印机）----
    bool pausedByApp = false;          // 当前打印机的暂停是否由本程序造成

    // 配置文件路径：可执行文件同目录的 config.ini
    static QString configPath();

    // 资源文件路径：可执行文件同目录的 assets/xxx（收款码图片等）
    static QString assetPath(const QString& fileName);

    // 读取配置；文件不存在时用内置默认值并写出一份，便于用户直接编辑
    static AppSettings load(QString* error = nullptr);

    // 保存配置（含运行期状态）
    bool save(QString* error = nullptr) const;
};

}  // namespace printpay
