#pragma once

#include "core/chart_project.h"

#include <QtWidgets/QWidget>

class QListView;

namespace infalsus::gui {

class EventsModel;

class EventsWidget final : public QWidget {
    Q_OBJECT

public:
    enum class EntryType { Timing, Lane, Speed };

    explicit EventsWidget(QWidget* parent = nullptr);

    void setEvents(const QVector<TimingPoint>& timingEvents, const QVector<LaneEvent>& laneEvents,
        const QVector<SpeedEvent>& speedEvents);
    void setSelectedTimingEvent(int index);
    void setSelectedTimingEvents(const QVector<int>& indexes);
    void setSelectedLaneEvent(int index);
    void setSelectedSpeedEvent(int index);
    void setSelectedEvents(const QVector<int>& timingIndexes, const QVector<int>& laneIndexes,
        const QVector<int>& speedIndexes);

signals:
    void addTimingRequested();
    void addLaneRequested();
    void addSpeedRequested();
    void eventSelected(EntryType type, int index);
    void timingEventsSelected(QVector<int> indexes);
    void eventsSelected(QVector<int> timingIndexes, QVector<int> laneIndexes, QVector<int> speedIndexes);
    void eventSeekRequested(EntryType type, int index);

private:
    void setSelected(EntryType type, int index);
    void setRowsSelected(const QSet<int>& rows, int anchor);
    void activateRow(int row, Qt::KeyboardModifiers modifiers, bool seek);
    void emitCurrentSelection(bool seek);

    QListView* m_points = nullptr;
    EventsModel* m_model = nullptr;
    QSet<int> m_selectedRows;
    int m_selectionAnchor = -1;
};

} // namespace infalsus::gui
