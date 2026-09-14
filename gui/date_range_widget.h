#pragma once

#include <QDate>
#include <QWidget>

class QComboBox;
class QDateEdit;
class QLabel;
class QPushButton;

// DateRangeWidget 提供日期范围选择：
//   最新（不限时间，按发布由新到旧）/ 最近一周 / 最近一个月 / 最近半年 /
//   自定义（两个 QDateEdit 日历窗，仅此项显示）。
// 预设直接换算成 [from, to]；自定义时用日期选择窗避免格式错误。
// 发出 rangeChanged(fromIso, toIso)，值为 "YYYY-MM-DD"，空串表示不限。
class DateRangeWidget : public QWidget
{
    Q_OBJECT
public:
    explicit DateRangeWidget(QWidget *parent = nullptr);

    QString fromIso() const; // "YYYY-MM-DD" 或空串
    QString toIso() const;

signals:
    void rangeChanged(const QString &fromIso, const QString &toIso);

private:
    enum Preset {
        Latest = 0,
        LastWeek = 1,
        LastMonth = 2,
        LastHalfYear = 3,
        Custom = 4,
    };

    QComboBox *m_preset;
    QDateEdit *m_from;
    QDateEdit *m_to;
    QLabel *m_sep;
    QPushButton *m_todayBtn;

    void applyPreset(int idx);
    void emitRange();
};
