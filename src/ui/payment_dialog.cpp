// =============================================================================
// 文件名：src/ui/payment_dialog.cpp
// 作用  ：收款弹窗实现。
// 说明  ：界面全部用代码构建（不依赖 .ui 文件），布局用 QGridLayout / QVBoxLayout，
//         字体用磅值设置以保证在 100%/125%/150% 缩放下都能看清。
// =============================================================================
#include "ui/payment_dialog.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFileInfo>
#include <QFont>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QSpinBox>
#include <QUrl>
#include <QVBoxLayout>

#include "core/billing.h"

namespace printpay {

PaymentDialog::PaymentDialog(const PrintJobInfo& job, int amountCents,
                             const AppSettings& settings, QWidget* parent)
    : QDialog(parent)
    , job_(job)
    , settings_(settings)
{
    setWindowTitle(QStringLiteral("打印完成 · 请扫码付款"));
    // 置顶 + 应用级模态：客户打印完，别的窗口不该盖住收款码
    setWindowFlags(windowFlags() | Qt::WindowStaysOnTopHint);
    setWindowModality(Qt::ApplicationModal);
    setMinimumWidth(760);

    QVBoxLayout* root = new QVBoxLayout(this);
    root->setSpacing(10);

    // ---------------- 标题与金额 ----------------
    QLabel* title = new QLabel(QStringLiteral("打印已完成，请扫码付款，谢谢！"), this);
    QFont titleFont = title->font();
    titleFont.setPointSize(20);
    titleFont.setBold(true);
    title->setFont(titleFont);
    title->setAlignment(Qt::AlignCenter);
    root->addWidget(title);

    amountLabel_ = new QLabel(this);
    QFont amountFont = amountLabel_->font();
    amountFont.setPointSize(34);
    amountFont.setBold(true);
    amountLabel_->setFont(amountFont);
    amountLabel_->setAlignment(Qt::AlignCenter);
    amountLabel_->setStyleSheet(QStringLiteral("color:#c81e1e;"));
    root->addWidget(amountLabel_);

    // ---------------- 作业信息与页数（可修正） ----------------
    QGroupBox* infoBox = new QGroupBox(QStringLiteral("本次打印明细"), this);
    QGridLayout* infoGrid = new QGridLayout(infoBox);

    infoGrid->addWidget(new QLabel(QStringLiteral("文档："), infoBox), 0, 0);
    QLabel* docLabel = new QLabel(job_.document.isEmpty() ? QStringLiteral("（未提供文档名）")
                                                         : job_.document, infoBox);
    docLabel->setWordWrap(true);
    infoGrid->addWidget(docLabel, 0, 1, 1, 3);

    infoGrid->addWidget(new QLabel(QStringLiteral("页数："), infoBox), 1, 0);
    pagesSpin_ = new QSpinBox(infoBox);
    pagesSpin_->setRange(0, 100000);
    pagesSpin_->setValue(job_.billablePages());   // 0 表示驱动没上报，需老板手工填
    pagesSpin_->setSuffix(QStringLiteral(" 页"));
    infoGrid->addWidget(pagesSpin_, 1, 1);

    infoGrid->addWidget(new QLabel(QStringLiteral("份数："), infoBox), 1, 2);
    infoGrid->addWidget(new QLabel(QStringLiteral("%1 份").arg(job_.copies), infoBox), 1, 3);

    // 驱动没上报页数时给醒目提示；正常情况下这行提示隐藏
    pagesHint_ = new QLabel(infoBox);
    pagesHint_->setStyleSheet(QStringLiteral("color:#c81e1e;"));
    pagesHint_->setWordWrap(true);
    infoGrid->addWidget(pagesHint_, 2, 0, 1, 4);
    root->addWidget(infoBox);

    // ---------------- 收款码区域 ----------------
    root->addWidget(createQrArea(), 1);

    // ---------------- 微信号 ----------------
    QLabel* contact = new QLabel(this);
    QFont contactFont = contact->font();
    contactFont.setPointSize(15);
    contactFont.setBold(true);
    contact->setFont(contactFont);
    contact->setAlignment(Qt::AlignCenter);
    contact->setText(QStringLiteral("如有问题或需要开票，请加微信：%1").arg(settings_.wechatId));
    contact->setStyleSheet(QStringLiteral("color:#1a4f8a;"));
    root->addWidget(contact);

    // ---------------- 按钮 ----------------
    QHBoxLayout* buttons = new QHBoxLayout;
    QPushButton* paidButton = new QPushButton(QStringLiteral("已完成付款"), this);
    QPushButton* unpaidButton = new QPushButton(QStringLiteral("暂未付款（记为未收款）"), this);
    paidButton->setMinimumHeight(46);
    unpaidButton->setMinimumHeight(46);
    QFont buttonFont = paidButton->font();
    buttonFont.setPointSize(12);
    paidButton->setFont(buttonFont);
    unpaidButton->setFont(buttonFont);
    buttons->addWidget(unpaidButton);
    buttons->addStretch(1);
    buttons->addWidget(paidButton);
    root->addLayout(buttons);

    connect(paidButton, &QPushButton::clicked, this, [this] {
        paid_ = true;
        accept();
    });
    connect(unpaidButton, &QPushButton::clicked, this, [this] {
        paid_ = false;
        accept();
    });
    connect(pagesSpin_, QOverload<int>::of(&QSpinBox::valueChanged),
            this, &PaymentDialog::onPagesChanged);

    updateAmountLabel();   // 初始化金额显示（含“页数未知”的提示）

    // 收款码放大显示：最大化而不是真全屏，保留标题栏与关闭按钮，避免把老板困在窗口里
    if (settings_.fullScreenPayment) {
        showMaximized();
    }
}

int PaymentDialog::pages() const
{
    return pagesSpin_->value();
}

bool PaymentDialog::paid() const
{
    return paid_;
}

void PaymentDialog::updateAmountLabel()
{
    const int pages = pagesSpin_->value();
    const int amount = computeAmountCents(pages, job_.copies,
                                         settings_.pricePerPageCents,
                                         settings_.minChargeCents);
    if (pages <= 0) {
        amountLabel_->setText(QStringLiteral("请填写页数"));
        pagesHint_->setText(QStringLiteral(
            "打印机驱动没有上报页数（有些驱动如此）。请按实际打印页数填写，金额会自动计算。"));
        pagesHint_->show();
        return;
    }

    if (job_.chargeable) {
        amountLabel_->setText(formatCents(amount));
        pagesHint_->setText(QStringLiteral("按每页 %1 计费，共 %2 页 × %3 份")
                                .arg(formatCents(settings_.pricePerPageCents))
                                .arg(pages)
                                .arg(job_.copies));
    } else {
        amountLabel_->setText(QStringLiteral("免费"));
        pagesHint_->setText(QStringLiteral("本次作业已设为免费，确认后直接记录流水。"));
    }
    pagesHint_->show();
}

void PaymentDialog::onPagesChanged()
{
    updateAmountLabel();
}

QPixmap PaymentDialog::loadQr(const QString& fileName) const
{
    const QString path = AppSettings::assetPath(fileName);
    QPixmap pixmap;
    if (!QFileInfo::exists(path)) {
        return pixmap;   // 调用方据此显示“图片缺失”的提示
    }
    if (!pixmap.load(path)) {
        return QPixmap();
    }
    return pixmap;
}

QWidget* PaymentDialog::createQrArea()
{
    QWidget* area = new QWidget(this);
    QHBoxLayout* layout = new QHBoxLayout(area);
    layout->setSpacing(16);

    // 收款码高度取可用屏幕高度的 42%，两张并排正好铺满屏幕宽度以内
    int targetHeight = 420;
    if (QScreen* screen = QApplication::primaryScreen()) {
        targetHeight = qMax(240, static_cast<int>(screen->availableGeometry().height() * 0.42));
    }

    const struct {
        const char* file;
        const char* caption;
    } items[] = {
        { "wechat_pay.png", "微信收款码" },
        { "alipay_pay.jpg", "支付宝收款码" },
    };

    for (const auto& item : items) {
        QWidget* box = new QWidget(area);
        QVBoxLayout* boxLayout = new QVBoxLayout(box);
        QLabel* caption = new QLabel(QString::fromUtf8(item.caption), box);
        QFont captionFont = caption->font();
        captionFont.setPointSize(14);
        captionFont.setBold(true);
        caption->setFont(captionFont);
        caption->setAlignment(Qt::AlignCenter);
        boxLayout->addWidget(caption);

        QLabel* image = new QLabel(box);
        image->setAlignment(Qt::AlignCenter);
        image->setMinimumSize(280, 240);
        image->setStyleSheet(QStringLiteral("border:1px solid #cccccc;background:#ffffff;"));

        const QPixmap pixmap = loadQr(QString::fromLatin1(item.file));
        if (pixmap.isNull()) {
            // 图片缺失时明确告诉老板去哪里放图片，而不是显示一片空白
            image->setText(QStringLiteral("未找到收款码图片：\nassets/%1\n\n请把收款码图片放到程序目录的 assets 文件夹里")
                               .arg(QString::fromLatin1(item.file)));
            image->setWordWrap(true);
        } else {
            image->setPixmap(pixmap.scaledToHeight(targetHeight, Qt::SmoothTransformation));
        }
        boxLayout->addWidget(image, 1);
        layout->addWidget(box, 1);
    }

    if (qrHint_ == nullptr) {
        qrHint_ = new QLabel(this);
    }
    return area;
}

}  // namespace printpay
