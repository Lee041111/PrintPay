// =============================================================================
// 文件名：src/core/settings.cpp
// 作用  ：配置读写实现。
// 注意  ：1) 读取时对每个值都做范围校正（例如轮询间隔被写成 0 会让定时器疯狂触发），
//            保证即使配置文件被手工改坏，程序也不会异常；
//         2) 首次运行时写出一份带默认值的 config.ini，用户可直接编辑。
// =============================================================================
#include "core/settings.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSettings>

namespace printpay {

QString AppSettings::configPath()
{
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("config.ini"));
}

QString AppSettings::assetPath(const QString& fileName)
{
    return QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("assets/") + fileName);
}

AppSettings AppSettings::load(QString* error)
{
    AppSettings settings;
    const QString path = configPath();
    const bool exists = QFileInfo::exists(path);

    // Qt 6 的 INI 读写固定使用 UTF-8，无需再设置编码（Qt5 的 setIniCodec 已移除）
    QSettings ini(path, QSettings::IniFormat);

    settings.printerName = ini.value(QStringLiteral("print/printerName"), QString()).toString();
    settings.pollIntervalMs = ini.value(QStringLiteral("print/pollIntervalMs"), 800).toInt();
    settings.pricePerPageCents = ini.value(QStringLiteral("billing/pricePerPageCents"), 10).toInt();
    settings.minChargeCents = ini.value(QStringLiteral("billing/minChargeCents"), 10).toInt();
    settings.wechatId = ini.value(QStringLiteral("contact/wechatId"), QString()).toString();

    settings.chargeOthers = ini.value(QStringLiteral("policy/chargeOthers"), true).toBool();
    settings.chargeAdmin = ini.value(QStringLiteral("policy/chargeAdmin"), false).toBool();
    settings.requireApprovalForOthers =
        ini.value(QStringLiteral("policy/requireApprovalForOthers"), true).toBool();
    settings.requireApprovalForAdmin =
        ini.value(QStringLiteral("policy/requireApprovalForAdmin"), false).toBool();
    settings.autoPaymentDialog = ini.value(QStringLiteral("policy/autoPaymentDialog"), true).toBool();
    settings.fullScreenPayment = ini.value(QStringLiteral("policy/fullScreenPayment"), false).toBool();

    // 界面与常驻方式（关闭按钮行为、启动是否隐藏窗口）
    settings.closeToTray = ini.value(QStringLiteral("ui/closeToTray"), true).toBool();
    settings.startMinimized = ini.value(QStringLiteral("ui/startMinimized"), false).toBool();

    settings.pausedByApp = ini.value(QStringLiteral("state/pausedByApp"), false).toBool();

    // ---- 范围校正：把明显不合理的配置拉回安全区间 ----
    if (settings.pollIntervalMs < 200) {
        settings.pollIntervalMs = 200;      // 再快也没意义，只会空耗 CPU
    }
    if (settings.pollIntervalMs > 10000) {
        settings.pollIntervalMs = 10000;
    }
    if (settings.pricePerPageCents < 0) {
        settings.pricePerPageCents = 0;
    }
    if (settings.minChargeCents < 0) {
        settings.minChargeCents = 0;
    }
    if (settings.wechatId.trimmed().isEmpty()) {
        settings.wechatId = QStringLiteral("（未填写微信号）");
    }

    if (error != nullptr) {
        error->clear();
        if (!exists) {
            *error = QStringLiteral("首次运行，已生成默认配置：%1").arg(path);
        }
    }
    if (!exists) {
        settings.save();   // 首次运行落一份默认配置，方便用户直接改
    }
    return settings;
}

bool AppSettings::save(QString* error) const
{
    const QString path = configPath();
    // 确保目录存在（正常情况下就是可执行文件目录）
    QDir().mkpath(QFileInfo(path).absolutePath());

    QSettings ini(path, QSettings::IniFormat);
    ini.setValue(QStringLiteral("print/printerName"), printerName);
    ini.setValue(QStringLiteral("print/pollIntervalMs"), pollIntervalMs);
    ini.setValue(QStringLiteral("billing/pricePerPageCents"), pricePerPageCents);
    ini.setValue(QStringLiteral("billing/minChargeCents"), minChargeCents);
    ini.setValue(QStringLiteral("contact/wechatId"), wechatId);
    ini.setValue(QStringLiteral("policy/chargeOthers"), chargeOthers);
    ini.setValue(QStringLiteral("policy/chargeAdmin"), chargeAdmin);
    ini.setValue(QStringLiteral("policy/requireApprovalForOthers"), requireApprovalForOthers);
    ini.setValue(QStringLiteral("policy/requireApprovalForAdmin"), requireApprovalForAdmin);
    ini.setValue(QStringLiteral("policy/autoPaymentDialog"), autoPaymentDialog);
    ini.setValue(QStringLiteral("policy/fullScreenPayment"), fullScreenPayment);
    ini.setValue(QStringLiteral("ui/closeToTray"), closeToTray);
    ini.setValue(QStringLiteral("ui/startMinimized"), startMinimized);
    ini.setValue(QStringLiteral("state/pausedByApp"), pausedByApp);
    ini.sync();

    if (ini.status() != QSettings::NoError) {
        if (error != nullptr) {
            *error = QStringLiteral("保存配置失败：%1（请检查程序目录是否可写）").arg(path);
        }
        return false;
    }
    return true;
}

}  // namespace printpay
