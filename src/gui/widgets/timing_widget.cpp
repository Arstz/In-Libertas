#include "gui/widgets/timing_widget.h"

#include <QtCore/QAbstractListModel>
#include <QtCore/QItemSelectionModel>
#include <QtGui/QGuiApplication>
#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>
#include <QtWidgets/QFrame>
#include <QtWidgets/QHBoxLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QListView>
#include <QtWidgets/QPushButton>
#include <QtWidgets/QStyledItemDelegate>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <functional>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <Windows.h>
#endif

namespace infalsus::gui {

namespace {

constexpr int kRowHeight = 26;

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

class EventsListView final : public QListView {
public:
    using ActivateCallback = std::function<void(int, Qt::KeyboardModifiers, bool)>;

    EventsListView(ActivateCallback activated, QWidget* parent)
        : QListView(parent), m_activated(std::move(activated)) {}

protected:
    void mousePressEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_activated(indexAt(event->position().toPoint()).row(), event->modifiers(), false);
            event->accept();
            return;
        }
        QListView::mousePressEvent(event);
    }

    void mouseDoubleClickEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton) {
            m_activated(indexAt(event->position().toPoint()).row(), event->modifiers(), true);
            event->accept();
            return;
        }
        QListView::mouseDoubleClickEvent(event);
    }

private:
    ActivateCallback m_activated;
};

class EventDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {0, kRowHeight};
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const bool selected = (option.state & QStyle::State_Selected) != 0;
        const QStringList fields = index.data(Qt::UserRole).toStringList();
        painter->save();
        painter->setClipRect(option.rect);
        painter->fillRect(option.rect, option.palette.brush(selected ? QPalette::Highlight : QPalette::Base));
        painter->setFont(option.font);
        const QRect contents = option.rect.adjusted(6, 1, -6, -1);
        const int fieldWidth = std::max(0, contents.width() - 20);
        const std::array<int, 3> widths{fieldWidth * 3 / 10, fieldWidth * 4 / 10,
            fieldWidth - fieldWidth * 3 / 10 - fieldWidth * 4 / 10};
        int left = contents.left();
        for (int field = 0; field < 3; ++field) {
            painter->setPen(option.palette.color(selected ? QPalette::HighlightedText : QPalette::Text));
            const QRect bounds(left, contents.top(), widths[field], contents.height());
            painter->drawText(bounds, Qt::AlignLeft | Qt::AlignVCenter,
                option.fontMetrics.elidedText(fields.value(field), Qt::ElideRight, bounds.width()));
            left += widths[field];
            if (field < 2) {
                painter->setPen(option.palette.color(QPalette::Mid));
                painter->drawLine(left + 4, contents.top(), left + 4, contents.bottom());
                left += 10;
            }
        }
        painter->restore();
    }
};

} // namespace

class EventsModel final : public QAbstractListModel {
public:
    struct Entry { EventsWidget::EntryType type; int index; qint64 timeMilliseconds; };

    using QAbstractListModel::QAbstractListModel;

    int rowCount(const QModelIndex& parent = {}) const override {
        return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
    }

    QVariant data(const QModelIndex& index, const int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()
            || (role != Qt::DisplayRole && role != Qt::UserRole)) {
            return {};
        }
        const Entry& entry = m_entries.at(index.row());
        QStringList fields;
        if (entry.type == EventsWidget::EntryType::Timing) {
            const TimingPoint& point = m_timingEvents.at(entry.index);
            fields = {QStringLiteral("%1 ms").arg(point.timeMilliseconds),
                QStringLiteral("%1 BPM").arg(point.beatsPerMinute, 0, 'f', 2),
                QStringLiteral("%1/%2").arg(point.timeSignatureNumerator).arg(point.timeSignatureDenominator)};
        } else if (entry.type == EventsWidget::EntryType::Lane) {
            const LaneEvent& event = m_laneEvents.at(entry.index);
            fields = {QStringLiteral("%1 ms").arg(event.timeMilliseconds), QStringLiteral("Lane %1").arg(event.lane),
                event.enabled ? QStringLiteral("ON") : QStringLiteral("OFF")};
        } else {
            const SpeedEvent& event = m_speedEvents.at(entry.index);
            fields = {QStringLiteral("%1 ms").arg(event.timeMilliseconds), QStringLiteral("SV"),
                QStringLiteral("%1x").arg(event.speed, 0, 'f', 3)};
        }
        return role == Qt::UserRole ? QVariant(fields) : QVariant(fields.join(QStringLiteral(" | ")));
    }

    void setEvents(const QVector<TimingPoint>& timingEvents, const QVector<LaneEvent>& laneEvents,
        const QVector<SpeedEvent>& speedEvents) {
        beginResetModel();
        m_timingEvents = timingEvents;
        m_laneEvents = laneEvents;
        m_speedEvents = speedEvents;
        m_entries.clear();
        m_entries.reserve(timingEvents.size() + laneEvents.size() + speedEvents.size());
        const auto append = [this](const auto& events, const EventsWidget::EntryType type) {
            m_rowsByType[static_cast<int>(type)].resize(events.size());
            for (int index = 0; index < events.size(); ++index) {
                m_entries.append({type, index, events.at(index).timeMilliseconds});
            }
        };
        append(timingEvents, EventsWidget::EntryType::Timing);
        append(laneEvents, EventsWidget::EntryType::Lane);
        append(speedEvents, EventsWidget::EntryType::Speed);
        std::stable_sort(m_entries.begin(), m_entries.end(), [](const Entry& first, const Entry& second) {
            return first.timeMilliseconds < second.timeMilliseconds;
        });
        for (int row = 0; row < m_entries.size(); ++row) {
            const Entry& entry = m_entries.at(row);
            m_rowsByType[static_cast<int>(entry.type)][entry.index] = row;
        }
        endResetModel();
    }

    const Entry& entry(const int row) const { return m_entries.at(row); }

    int rowFor(const EventsWidget::EntryType type, const int index) const {
        const auto& rows = m_rowsByType[static_cast<int>(type)];
        return index >= 0 && index < rows.size() ? rows.at(index) : -1;
    }

private:
    QVector<Entry> m_entries;
    std::array<QVector<int>, 3> m_rowsByType;
    QVector<TimingPoint> m_timingEvents;
    QVector<LaneEvent> m_laneEvents;
    QVector<SpeedEvent> m_speedEvents;
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

    m_model = new EventsModel(this);
    m_points = new EventsListView([this](const int row, const Qt::KeyboardModifiers modifiers, const bool seek) {
        activateRow(row, modifiers, seek);
    }, this);
    m_points->setModel(m_model);
    m_points->setItemDelegate(new EventDelegate(m_points));
    m_points->setUniformItemSizes(true);
    m_points->setSelectionMode(QAbstractItemView::NoSelection);
    m_points->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_points->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_points->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_points->setFrameShape(QFrame::StyledPanel);

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
    QSet<int> selected;
    int current = -1;
    const auto select = [&](const EntryType type, const QVector<int>& indexes) {
        for (const int index : indexes) {
            const int row = m_model->rowFor(type, index);
            if (row >= 0) {
                selected.insert(row);
                current = std::max(current, row);
            }
        }
    };
    select(EntryType::Timing, timingIndexes);
    select(EntryType::Lane, laneIndexes);
    select(EntryType::Speed, speedIndexes);
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
    m_selectedRows.clear();
    m_selectionAnchor = -1;
    m_model->setEvents(timingEvents, laneEvents, speedEvents);
}

void EventsWidget::setSelected(const EntryType type, const int index) {
    const int row = m_model->rowFor(type, index);
    setRowsSelected(row >= 0 ? QSet<int>{row} : QSet<int>{}, row);
}

void EventsWidget::setRowsSelected(const QSet<int>& rows, const int anchor) {
    m_selectionAnchor = anchor;
    if (m_selectedRows == rows) {
        return;
    }
    m_selectedRows = rows;
    QList<int> ordered = rows.values();
    std::sort(ordered.begin(), ordered.end());
    QItemSelection selection;
    for (int index = 0; index < ordered.size();) {
        const int first = ordered.at(index);
        int last = first;
        while (++index < ordered.size() && ordered.at(index) == last + 1) {
            last = ordered.at(index);
        }
        selection.select(m_model->index(first), m_model->index(last));
    }
    m_points->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect);
}

void EventsWidget::activateRow(const int row, const Qt::KeyboardModifiers modifiers, const bool seek) {
    if (row < 0 || row >= m_model->rowCount()) {
        return;
    }
    QSet<int> selected = m_selectedRows;
    int anchor = row;
    if (!seek && isShiftHeld(modifiers) && m_selectionAnchor >= 0) {
        selected.clear();
        for (int index = std::min(m_selectionAnchor, row); index <= std::max(m_selectionAnchor, row); ++index) {
            selected.insert(index);
        }
        anchor = m_selectionAnchor;
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
    if (m_selectedRows.size() > 1) {
        QVector<int> timingIndexes;
        QVector<int> laneIndexes;
        QVector<int> speedIndexes;
        for (const int row : m_selectedRows) {
            const auto& entry = m_model->entry(row);
            if (entry.type == EntryType::Timing) {
                timingIndexes.append(entry.index);
            } else if (entry.type == EntryType::Lane) {
                laneIndexes.append(entry.index);
            } else {
                speedIndexes.append(entry.index);
            }
        }
        std::sort(timingIndexes.begin(), timingIndexes.end());
        std::sort(laneIndexes.begin(), laneIndexes.end());
        std::sort(speedIndexes.begin(), speedIndexes.end());
        emit eventsSelected(std::move(timingIndexes), std::move(laneIndexes), std::move(speedIndexes));
        return;
    }
    const auto entry = m_model->entry(*m_selectedRows.cbegin());
    if (seek) {
        emit eventSeekRequested(entry.type, entry.index);
    } else {
        emit eventSelected(entry.type, entry.index);
    }
}

} // namespace infalsus::gui
