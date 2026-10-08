#pragma once

#include "gui/state/editor_state.h"

#include <QtCore/QVariant>
#include <QtWidgets/QWidget>

class QFormLayout;

namespace infalsus::gui {

class PropertiesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);

    void setSelection(const EditorState& state);
    void setEventOffset(qint64 offsetMilliseconds);

signals:
    void eventOffsetCurrentTimeRequested();
    void hitObjectsEditRequested(QVector<int> indexes, QVector<ChartNote> hitObjects);
    void eventsEditRequested(QVector<TimingPoint> timingPoints, QVector<LaneEvent> laneEvents,
        QVector<SpeedEvent> speedEvents);

private:
    enum class HitObjectProperty {
        GroupId, Offset, End, FirstLane, LastLane, StartPosition, StartWidth, EndPosition, EndWidth, Encoding,
    };
    enum class EventProperty {
        Offset, Bpm, Numerator, Denominator, Lane, Enabled, Speed,
    };

    void clearRows();
    void addReadOnlyRow(const QString& label, const QString& value);
    void showHitObjects();
    void showEvents();
    void applyHitObjectProperty(HitObjectProperty property, const QVariant& value);
    void applyEventProperty(EventProperty property, const QVariant& value);

    QFormLayout* m_layout = nullptr;
    QVector<int> m_hitObjectIndexes;
    QVector<ChartNote> m_hitObjects;
    QVector<TimingPoint> m_timingPoints;
    QVector<LaneEvent> m_laneEvents;
    QVector<SpeedEvent> m_speedEvents;
    bool m_populating = false;
};

} // namespace infalsus::gui
