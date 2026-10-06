#pragma once

#include "gui/state/editor_state.h"

#include <QtWidgets/QWidget>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QSpinBox;

namespace infalsus::gui {

class PropertiesPanel final : public QWidget {
    Q_OBJECT

public:
    explicit PropertiesPanel(QWidget* parent = nullptr);

    void setSelection(const EditorState& state);

signals:
    void hitObjectEditRequested(int index, const ChartNote& hitObject);
    void timingPointEditRequested(int index, const TimingPoint& timingPoint);
    void laneEventEditRequested(int index, const LaneEvent& laneEvent);
    void speedEventEditRequested(int index, const SpeedEvent& speedEvent);

private:
    void clearRows();
    void addReadOnlyRow(const QString& label, const QString& value);
    void showHitObject(int index, const ChartNote& hitObject);
    void showTimingPoint(int index, const TimingPoint& timingPoint);
    void showLaneEvent(int index, const LaneEvent& laneEvent);
    void showSpeedEvent(int index, const SpeedEvent& speedEvent);
    void commitHitObjectEdit();
    void commitTimingPointEdit();
    void commitLaneEventEdit();
    void commitSpeedEventEdit();

    QFormLayout* m_layout = nullptr;
    QSpinBox* m_offsetEditor = nullptr;
    QSpinBox* m_endEditor = nullptr;
    QSpinBox* m_laneEditor = nullptr;
    QSpinBox* m_endLaneEditor = nullptr;
    QDoubleSpinBox* m_startCoordinateEditor = nullptr;
    QDoubleSpinBox* m_startWidthEditor = nullptr;
    QDoubleSpinBox* m_endCoordinateEditor = nullptr;
    QDoubleSpinBox* m_endWidthCoordinateEditor = nullptr;
    QSpinBox* m_auxiliaryEditor = nullptr;
    QDoubleSpinBox* m_bpmEditor = nullptr;
    QSpinBox* m_numeratorEditor = nullptr;
    QComboBox* m_denominatorEditor = nullptr;
    QComboBox* m_laneEventStateEditor = nullptr;
    QDoubleSpinBox* m_speedEditor = nullptr;
    ChartNote m_hitObject;
    TimingPoint m_timingPoint;
    LaneEvent m_laneEvent;
    SpeedEvent m_speedEvent;
    int m_hitObjectIndex = -1;
    int m_timingPointIndex = -1;
    int m_laneEventIndex = -1;
    int m_speedEventIndex = -1;
    bool m_populating = false;
};

} // namespace infalsus::gui
