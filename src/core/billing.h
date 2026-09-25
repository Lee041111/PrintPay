// =============================================================================
// 文件名：src/core/billing.h
// 作用  ：计费算法（纯函数，不依赖界面与数据库，方便单元测试）。
// 设计  ：金额一律用“分”做整数运算，绝不使用浮点数 —— 0.1 元这类小数在
//         二进制浮点里无法精确表示，乘页数后会出现“0.30000000000000004 元”这种问题。
// =============================================================================
#pragma once

#include <QString>

namespace printpay {

// 把“元”文本转成“分”。支持 "0.1"、"0.10"、"1"、"0.05" 等写法；
// 非法输入（空、负数、超过两位小数、含非数字字符）返回 -1，调用方据此提示用户。
int parseYuanToCents(const QString& text);

// 把“分”格式化成“x.xx 元”文本（用于界面显示）。
QString formatCents(int cents);

// 计算本次打印应收金额（分）。
//   pages             ：页数（0 表示未知，返回 0 由界面提示老板手工填）
//   copies            ：份数（<=0 按 1 份处理）
//   pricePerPageCents ：每页单价（分）
//   minChargeCents    ：最低消费（分），空单也会按最低消费收取
// 结果做了溢出与负数保护：页数或单价异常时返回 0，绝不会算出负数金额。
int computeAmountCents(int pages, int copies, int pricePerPageCents, int minChargeCents);

}  // namespace printpay
