#include "date_range_widget.h"

#include <QComboBox>
#include <QDateEdit>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

DateRangeWidget::DateRangeWidget(QWidget *parent)
    : QWidget(parent)
{
    m_preset = new QComboBox(this);
    m_preset->addItem(tr("Latest"), Latest);
    m_preset->addItem(tr("Last Week"), LastWeek);
    m_preset->addItem(tr("Last Month"), LastMonth);
    m_preset->addItem(tr("Last 6 Months"), LastHalfYear);
    m_preset->addItem(tr("Custom range..."), Custom);

    m_from = new QDateEdit(this);
    m_to = new QDateEdit(this);
    m_from->setCalendarPopup(true);
    m_to->setCalendarPopup(true);
    m_from->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_to->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
    m_from->setMinimumDate(QDate(2000, 1, 1));
    m_from->setMaximumDate(QDate::currentDate());
    m_to->setMinimumDate(QDate(2000, 1, 1));
    m_to->setMaximumDate(QDate::currentDate());
    m_sep = new QLabel(tr("to"), this);
    m_todayBtn = new QPushButton(tr("Today"), this);

    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_preset);
    layout->addWidget(m_from);
    layout->addWidget(m_sep);
    layout->addWidget(m_to);
    layout->addWidget(m_todayBtn);
    layout->addStretch();

    // 今日：起止日期都设为今天，查当天发布。
    connect(m_todayBtn, &QPushButton::clicked, this, [this]() {
        const QDate today = QDate::currentDate();
        QSignalBlocker b1(m_from);
        QSignalBlocker b2(m_to);
        m_from->setDate(today);
        m_to->setDate(today);
        emitRange();
    });

    // 预设切换时联动日期；自定义时允许直接改日期窗。
    connect(m_preset, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx) { applyPreset(idx); });
    connect(m_from, &QDateEdit::dateChanged, this, [this]() {
        if (m_preset->currentData().toInt() == Custom)
            emitRange();
    });
    connect(m_to, &QDateEdit::dateChanged, this, [this]() {
        if (m_preset->currentData().toInt() == Custom)
            emitRange();
    });

    // 默认"最新"：不限时间范围，直接按发布由新到旧排列。
    m_preset->setCurrentIndex(Latest);
    applyPreset(Latest);
}

QString DateRangeWidget::fromIso() const
{
    if (m_preset->currentData().toInt() == Latest)
        return QString();
    return m_from->date().toString(QStringLiteral("yyyy-MM-dd"));
}

QString DateRangeWidget::toIso() const
{
    if (m_preset->currentData().toInt() == Latest)
        return QString();
    return m_to->date().toString(QStringLiteral("yyyy-MM-dd"));
}

void DateRangeWidget::applyPreset(int idx)
{
    const QDate today = QDate::currentDate();
    switch (idx) {
    case Latest:
        // 不设置任何范围；日期窗保持隐藏。
        break;
    case LastWeek:
        m_from->setDate(today.addDays(-7));
        m_to->setDate(today);
        break;
    case LastMonth:
        m_from->setDate(today.addMonths(-1));
        m_to->setDate(today);
        break;
    case LastHalfYear:
        m_from->setDate(today.addMonths(-6));
        m_to->setDate(today);
        break;
    case Custom:
    default:
        // 保留当前日期值，用户可直接改。
        break;
    }
    // 只有"自定义范围"才显示起止日期选择窗与"今日"按钮。
    const bool showEdits = (idx == Custom);
    m_from->setVisible(showEdits);
    m_to->setVisible(showEdits);
    m_sep->setVisible(showEdits);
    m_todayBtn->setVisible(showEdits);
    emitRange();
}

void DateRangeWidget::emitRange()
{
    emit rangeChanged(fromIso(), toIso());
}
