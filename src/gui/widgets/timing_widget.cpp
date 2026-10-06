#include "gui/widgets/timing_widget.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QScrollArea>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <functional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#endif

namespace infalsus::gui {

namespace {

[[nodiscard]] bool isControlHeld(const Qt::KeyboardModifiers modifiers) {
    if ((modifiers & Qt::ControlModifier) != 0
        || (QGuiApplication::queryKeyboardModifiers() & Qt::ControlModifier) != 0) {
        return true;
    }
#ifdef Q_OS_WIN
    return (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
#else
    return false;
#endif
}

[[nodiscard]] bool isShiftHeld(const Qt::KeyboardModifiers modifiers) {
    if ((modifiers & Qt::ShiftModifier) != 0
        || (QGuiApplication::queryKeyboardModifiers() & Qt::ShiftModifier) != 0) {
        return true;
    }
#ifdef Q_OS_WIN
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return false;
#endif
}

void addDivider(QHBoxLayout* layout, QWidget* parent) {
    auto* divider = new QFrame(parent);
    divider->setFrameShape(QFrame::VLine);
    divider->setFrameShadow(QFrame::Sunken);
    divider->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(divider);
}

void addField(QHBoxLayout* layout, QWidget* parent, const QString& text, const int stretch) {
    auto* label = new QLabel(text, parent);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setTextInteractionFlags(Qt::NoTextInteraction);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(label, stretch);
}

} // namespace

class EventRow final : public QWidget {
public:
    using ActivateCallback = std::function<void(EventRow*, Qt::KeyboardModifiers, bool)>;

    EventRow(const EventsWidget::EntryType type, const int index, const QStringList& fields,
        ActivateCallback activated, QWidget* parent)
        : QWidget(parent)
        , m_type(type)
        , m_index(index)
        , m_activated(std::move(activated)) {
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(6, 1, 6, 1);
        layout->setSpacing(4);
        addField(layout, this, fields.value(0), 3);
        addDivider(layout, this);
        addField(layout, this, fields.value(1), 4);
        addDivider(layout, this);
        addField(layout, this, fields.value(2), 3);
        setMinimumHeight(26);
        setAutoFillBackground(true);
        setSelected(false);
    }

    [[nodiscard]] EventsWidget::EntryType type() const { return m_type; }
    [[nodiscard]] int index() const { return m_index; }

    void setSelected(const bool selected) {
        QPalette rowPalette = palette();
        rowPalette.setColor(QPalette::Window, rowPalette.color(selected ? QPalette::Highlight : QPalette::Base));
        rowPalette.setColor(QPalette::WindowText, rowPalette.color(selected ? QPalette::HighlightedText : QPalette::Text));
        setPalette(rowPalette);
    }

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && m_activated) {
            m_activated(this, event->modifiers(), false);
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && m_activated) {
            m_activated(this, event->modifiers(), true);
            event->accept();
            return;
        }
        QWidget::mouseDoubleClickEvent(event);
    }

private:
    EventsWidget::EntryType m_type;
    int m_index = -1;
    ActivateCallback m_activated;
};

EventsWidget::EventsWidget(QWidget* parent)
    : QWidget(parent) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(6, 1, 6, 1);
    headerLayout->setSpacing(4);
    addField(headerLayout, header, QStringLiteral("Time"), 3);
    addDivider(headerLayout, header);
    addField(headerLayout, header, QStringLiteral("Entry"), 4);
    addDivider(headerLayout, header);
    addField(headerLayout, header, QStringLiteral("Value"), 3);

    m_points = new QScrollArea(this);
    m_points->setWidgetResizable(true);
    m_points->setFrameShape(QFrame::StyledPanel);
    auto* rowsHost = new QWidget(m_points);
    m_rowsLayout = new QVBoxLayout(rowsHost);
    m_rowsLayout->setContentsMargins(0, 0, 0, 0);
    m_rowsLayout->setSpacing(0);
    m_rowsLayout->addStretch(1);
    m_points->setWidget(rowsHost);

    auto* buttons = new QHBoxLayout();
    auto* addTimingButton = new QPushButton(QStringLiteral("Add timing event"), this);
    auto* addLaneButton = new QPushButton(QStringLiteral("Add lane event"), this);
    auto* addSpeedButton = new QPushButton(QStringLiteral("Add SV event"), this);
    buttons->addWidget(addTimingButton);
    buttons->addWidget(addLaneButton);
    buttons->addWidget(addSpeedButton);
    layout->addWidget(header);
    layout->addWidget(m_points, 1);
    layout->addLayout(buttons);

    connect(addTimingButton, &QPushButton::clicked, this, &EventsWidget::addTimingRequested);
    connect(addLaneButton, &QPushButton::clicked, this, &EventsWidget::addLaneRequested);
    connect(addSpeedButton, &QPushButton::clicked, this, &EventsWidget::addSpeedRequested);
}

void EventsWidget::setSelectedTimingEvent(const int index) {
    setSelectedTimingEvents({index});
}

void EventsWidget::setSelectedTimingEvents(const QVector<int>& indexes) {
    setSelectedEvents(indexes, {}, {});
}

void EventsWidget::setSelectedEvents(const QVector<int>& timingIndexes, const QVector<int>& laneIndexes,
    const QVector<int>& speedIndexes) {
    QSet<EventRow*> selected;
    EventRow* current = nullptr;
    for (EventRow* row : m_rows) {
        const bool isSelected = (row->type() == EntryType::Timing && timingIndexes.contains(row->index()))
            || (row->type() == EntryType::Lane && laneIndexes.contains(row->index()))
            || (row->type() == EntryType::Speed && speedIndexes.contains(row->index()));
        if (isSelected) {
            selected.insert(row);
            current = row;
        }
    }
    setRowsSelected(selected, current);
}

void EventsWidget::setSelectedLaneEvent(const int index) {
    setSelected(EntryType::Lane, index);
}

void EventsWidget::setSelectedSpeedEvent(const int index) {
    setSelected(EntryType::Speed, index);
}

void EventsWidget::setEvents(const QVector<TimingPoint>& timingEvents, const QVector<LaneEvent>& laneEvents,
    const QVector<SpeedEvent>& speedEvents) {
    while (QLayoutItem* item = m_rowsLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    m_rows.clear();
    m_selectedRows.clear();
    m_selectionAnchor = nullptr;

    struct Entry { EntryType type; int index; qint64 timeMilliseconds; };
    QVector<Entry> entries;
    entries.reserve(timingEvents.size() + laneEvents.size() + speedEvents.size());
    for (int index = 0; index < timingEvents.size(); ++index) {
        entries.append({EntryType::Timing, index, timingEvents.at(index).timeMilliseconds});
    }
    for (int index = 0; index < laneEvents.size(); ++index) {
        entries.append({EntryType::Lane, index, laneEvents.at(index).timeMilliseconds});
    }
    for (int index = 0; index < speedEvents.size(); ++index) {
        entries.append({EntryType::Speed, index, speedEvents.at(index).timeMilliseconds});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& first, const Entry& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });

    for (const Entry& entry : entries) {
        QStringList fields;
        if (entry.type == EntryType::Timing) {
            const TimingPoint& point = timingEvents.at(entry.index);
            fields = {QStringLiteral("%1 ms").arg(point.timeMilliseconds),
                QStringLiteral("%1 BPM").arg(point.beatsPerMinute, 0, 'f', 2),
                QStringLiteral("%1/%2").arg(point.timeSignatureNumerator).arg(point.timeSignatureDenominator)};
        } else if (entry.type == EntryType::Lane) {
            const LaneEvent& event = laneEvents.at(entry.index);
            fields = {QStringLiteral("%1 ms").arg(event.timeMilliseconds), QStringLiteral("Lane %1").arg(event.lane),
                event.enabled ? QStringLiteral("ON") : QStringLiteral("OFF")};
        } else {
            const SpeedEvent& event = speedEvents.at(entry.index);
            fields = {QStringLiteral("%1 ms").arg(event.timeMilliseconds), QStringLiteral("SV"),
                QStringLiteral("%1x").arg(event.speed, 0, 'f', 3)};
        }
        auto* row = new EventRow(entry.type, entry.index, fields,
            [this](EventRow* activated, const Qt::KeyboardModifiers modifiers, const bool seek) {
                activateRow(activated, modifiers, seek);
            }, m_points->widget());
        m_rows.append(row);
        m_rowsLayout->addWidget(row);
    }
    m_rowsLayout->addStretch(1);
}

void EventsWidget::setSelected(const EntryType type, const int index) {
    for (EventRow* row : m_rows) {
        if (row->type() == type && row->index() == index) {
            setRowsSelected({row}, row);
            return;
        }
    }
    setRowsSelected({}, nullptr);
}

void EventsWidget::setRowsSelected(const QSet<EventRow*>& rows, EventRow* anchor) {
    m_selectedRows = rows;
    m_selectionAnchor = anchor;
    for (EventRow* row : m_rows) {
        row->setSelected(m_selectedRows.contains(row));
    }
}

void EventsWidget::activateRow(EventRow* row, const Qt::KeyboardModifiers modifiers, const bool seek) {
    if (row == nullptr) {
        return;
    }

    QSet<EventRow*> selected = m_selectedRows;
    EventRow* anchor = row;
    if (!seek && isShiftHeld(modifiers) && m_selectionAnchor != nullptr) {
        const int first = m_rows.indexOf(m_selectionAnchor);
        const int last = m_rows.indexOf(row);
        if (first >= 0 && last >= 0) {
            selected.clear();
            for (int index = std::min(first, last); index <= std::max(first, last); ++index) {
                selected.insert(m_rows.at(index));
            }
            anchor = m_selectionAnchor;
        }
    } else if (!seek && isControlHeld(modifiers)) {
        if (selected.contains(row)) {
            selected.remove(row);
        } else {
            selected.insert(row);
        }
    } else {
        selected = {row};
    }
    setRowsSelected(selected, anchor);
    emitCurrentSelection(seek);
}

void EventsWidget::emitCurrentSelection(const bool seek) {
    if (m_selectedRows.isEmpty()) {
        emit timingEventsSelected({});
        return;
    }

    EventRow* current = nullptr;
    for (EventRow* row : m_rows) {
        if (!m_selectedRows.contains(row)) {
            continue;
        }
        current = row;
    }
    if (m_selectedRows.size() > 1) {
        QVector<int> timingIndexes;
        QVector<int> laneIndexes;
        QVector<int> speedIndexes;
        for (EventRow* row : m_rows) {
            if (!m_selectedRows.contains(row)) {
                continue;
            }
            if (row->type() == EntryType::Timing) {
                timingIndexes.append(row->index());
            } else if (row->type() == EntryType::Lane) {
                laneIndexes.append(row->index());
            } else {
                speedIndexes.append(row->index());
            }
        }
        std::sort(timingIndexes.begin(), timingIndexes.end());
        std::sort(laneIndexes.begin(), laneIndexes.end());
        std::sort(speedIndexes.begin(), speedIndexes.end());
        emit eventsSelected(std::move(timingIndexes), std::move(laneIndexes), std::move(speedIndexes));
        return;
    }
    if (current == nullptr) {
        return;
    }
    if (seek) {
        emit eventSeekRequested(current->type(), current->index());
    } else {
        emit eventSelected(current->type(), current->index());
    }
}

} // namespace infalsus::gui
