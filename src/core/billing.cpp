// =============================================================================
// 文件名：src/core/billing.cpp
// 作用  ：计费算法实现。
// 注意  ：全部用整数（分）计算；对非法输入一律“安全失败”，宁可返回 -1/0 让界面
//         提示，也不要算出错误金额。
// =============================================================================
#include "core/billing.h"

#include <QRegularExpression>

#include <limits>

namespace printpay {

int parseYuanToCents(const QString& text)
{
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        return -1;
    }
    // 只接受最多两位小数的非负数字，例如 0、0.1、0.10、1、12.5、99.99
    static const QRegularExpression pattern(QStringLiteral("^\\d+(\\.\\d{1,2})?$"));
    if (!pattern.match(trimmed).hasMatch()) {
        return -1;
    }

    const int dot = trimmed.indexOf(QLatin1Char('.'));
    const QString yuanPart = dot < 0 ? trimmed : trimmed.left(dot);
    QString centPart = dot < 0 ? QStringLiteral("0") : trimmed.mid(dot + 1);
    if (centPart.size() == 1) {
        centPart += QLatin1Char('0');   // "0.1" 元 = 10 分
    }

    bool ok = false;
    const qlonglong yuan = yuanPart.toLongLong(&ok);
    if (!ok) {
        return -1;
    }
    const qlonglong cents = centPart.toLongLong(&ok);
    if (!ok) {
        return -1;
    }

    const qlonglong total = yuan * 100 + cents;
    if (total > 100000000LL) {   // 超过一亿元视为输入错误，避免溢出
        return -1;
    }
    return static_cast<int>(total);
}

QString formatCents(int cents)
{
    if (cents < 0) {
        cents = 0;
    }
    return QStringLiteral("%1.%2 元")
        .arg(cents / 100)
        .arg(cents % 100, 2, 10, QLatin1Char('0'));
}

int computeAmountCents(int pages, int copies, int pricePerPageCents, int minChargeCents)
{
    if (pages <= 0 || pricePerPageCents <= 0) {
        return 0;   // 页数未知或单价未配置：金额为 0，由界面提示老板手工确认
    }
    if (copies <= 0) {
        copies = 1;
    }
    if (minChargeCents < 0) {
        minChargeCents = 0;
    }

    const qlonglong perCopy = static_cast<qlonglong>(pages) * pricePerPageCents;
    const qlonglong total = perCopy * copies;
    if (total > std::numeric_limits<int>::max()) {   // 极端页数保护
        return std::numeric_limits<int>::max();
    }
    const qlonglong amount = total > minChargeCents ? total : minChargeCents;
    return static_cast<int>(amount);
}

}  // namespace printpay
