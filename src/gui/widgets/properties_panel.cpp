#include "gui/widgets/properties_panel.h"

#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QSpinBox>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace infalsus::gui {

namespace {

constexpr int kMaximumOffsetMilliseconds = 36'000'000;
constexpr int kMaximumAuxiliaryValue = std::numeric_limits<int>::max();
constexpr int kFirstCentralLane = 1;
constexpr int kLastCentralLane = 4;
constexpr int kLastFloorLane = 5;
constexpr double kCoordinateMinimum = 0.0;
constexpr double kCoordinateMaximum = 1.0;
constexpr double kMinimumSkyWidth = 0.01;
constexpr double kMaximumSkyWidth = 1.0;
constexpr double kMinimumBpm = 1.0;
constexpr double kMaximumBpm = 1000.0;
constexpr int kMinimumSignatureNumerator = 1;
constexpr int kMaximumSignatureNumerator = 32;

[[nodiscard]] QString hitObjectName(const ChartNote& hitObject) {
    switch (hitObject.kind) {
    case NoteKind::Tap:
        return QStringLiteral("Note");
    case NoteKind::Hold:
        return QStringLiteral("Hold");
    case NoteKind::Flick:
        return QStringLiteral("Flick");
    case NoteKind::Sky:
        return QStringLiteral("Zone");
    case NoteKind::Unknown:
        return QStringLiteral("Unknown");
    }

    return QStringLiteral("Unknown");
}

[[nodiscard]] int floorLane(const FloorSide side, const double coordinate) {
    if (side == FloorSide::LeftOuter) {
        return 0;
    }
    if (side == FloorSide::RightOuter) {
        return kLastFloorLane;
    }

    return std::clamp(static_cast<int>(std::lround(coordinate * 4.0)), kFirstCentralLane, kLastCentralLane);
}

void setFloorSpan(ChartNote& hitObject, const int firstLane, const int lastLane) {
    const int clampedFirstLane = std::clamp(firstLane, 0, kLastFloorLane);
    if (clampedFirstLane == 0 || clampedFirstLane == kLastFloorLane) {
        hitObject.side = clampedFirstLane == 0 ? FloorSide::LeftOuter : FloorSide::RightOuter;
        hitObject.startX = 0.0;
        hitObject.endX = 0.0;
        hitObject.startWidth = 1.0;
        hitObject.endWidth = 1.0;

        return;
    }

    hitObject.side = FloorSide::Central;
    const int clampedLastLane = std::clamp(lastLane, clampedFirstLane, kLastCentralLane);
    const int laneCount = clampedLastLane - clampedFirstLane + 1;
    const double coordinate = static_cast<double>(clampedFirstLane) / 4.0;
    const double normalizedWidth = static_cast<double>(laneCount) / 4.0;
    hitObject.startX = coordinate;
    hitObject.endX = coordinate;
    hitObject.startWidth = normalizedWidth;
    hitObject.endWidth = normalizedWidth;
}

} // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent) {
    m_layout = new QFormLayout(this);
    clearRows();
    addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("No selection"));
}

void PropertiesPanel::setSelection(const EditorState& state) {
    const QVector<int>& selectedHitObjects = state.selectedHitObjects();

    m_populating = true;
    clearRows();
    if (!selectedHitObjects.isEmpty()) {
        if (selectedHitObjects.size() == 1) {
            const int index = selectedHitObjects.front();
            showHitObject(index, state.chart().notes.at(index));
        } else {
            addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("%1 hit objects").arg(selectedHitObjects.size()));
        }
        m_populating = false;

        return;
    }

    const int selectedEventCount = std::max(state.selectedEventCount(), static_cast<int>(state.selectedTimingPoints().size()));
    if (selectedEventCount > 1) {
        addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("%1 events").arg(selectedEventCount));
        m_populating = false;
        return;
    }

    const int timingPointIndex = state.selectedTimingPoint();
    if (timingPointIndex >= 0 && timingPointIndex < state.timingPoints().size()) {
        showTimingPoint(timingPointIndex, state.timingPoints().at(timingPointIndex));
    } else {
        const int laneEventIndex = state.selectedLaneEvent();
        if (laneEventIndex >= 0 && laneEventIndex < state.laneEvents().size()) {
            showLaneEvent(laneEventIndex, state.laneEvents().at(laneEventIndex));
        } else {
            const int speedEventIndex = state.selectedSpeedEvent();
            if (speedEventIndex >= 0 && speedEventIndex < state.speedEvents().size()) {
                showSpeedEvent(speedEventIndex, state.speedEvents().at(speedEventIndex));
            } else {
                addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("No selection"));
            }
        }
    }
    m_populating = false;
}

void PropertiesPanel::clearRows() {
    while (m_layout->rowCount() > 0) {
        m_layout->removeRow(0);
    }
    m_offsetEditor = nullptr;
    m_endEditor = nullptr;
    m_laneEditor = nullptr;
    m_endLaneEditor = nullptr;
    m_startCoordinateEditor = nullptr;
    m_startWidthEditor = nullptr;
    m_endCoordinateEditor = nullptr;
    m_endWidthCoordinateEditor = nullptr;
    m_auxiliaryEditor = nullptr;
    m_bpmEditor = nullptr;
    m_numeratorEditor = nullptr;
    m_denominatorEditor = nullptr;
    m_laneEventStateEditor = nullptr;
    m_speedEditor = nullptr;
    m_hitObjectIndex = -1;
    m_timingPointIndex = -1;
    m_laneEventIndex = -1;
    m_speedEventIndex = -1;
}

void PropertiesPanel::addReadOnlyRow(const QString& label, const QString& value) {
    auto* valueLabel = new QLabel(value, this);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_layout->addRow(label, valueLabel);
}

void PropertiesPanel::showHitObject(const int index, const ChartNote& hitObject) {
    const bool isSkyObject = hitObject.side == FloorSide::Sky;

    m_hitObjectIndex = index;
    m_hitObject = hitObject;
    addReadOnlyRow(QStringLiteral("Type"), hitObjectName(hitObject));
    addReadOnlyRow(QStringLiteral("ID"), QString::number(hitObject.id));
    m_offsetEditor = new QSpinBox(this);
    m_offsetEditor->setRange(0, kMaximumOffsetMilliseconds);
    m_offsetEditor->setSuffix(QStringLiteral(" ms"));
    m_offsetEditor->setValue(static_cast<int>(std::min<qint64>(hitObject.startMilliseconds, kMaximumOffsetMilliseconds)));
    m_layout->addRow(QStringLiteral("Offset"), m_offsetEditor);
    m_endEditor = new QSpinBox(this);
    m_endEditor->setRange(0, kMaximumOffsetMilliseconds);
    m_endEditor->setSuffix(QStringLiteral(" ms"));
    m_endEditor->setValue(static_cast<int>(std::min<qint64>(hitObject.endMilliseconds, kMaximumOffsetMilliseconds)));
    m_layout->addRow(QStringLiteral("End"), m_endEditor);

    if (isSkyObject) {
        m_startCoordinateEditor = new QDoubleSpinBox(this);
        m_startCoordinateEditor->setRange(kCoordinateMinimum, kCoordinateMaximum);
        m_startCoordinateEditor->setDecimals(3);
        m_startCoordinateEditor->setSingleStep(0.025);
        m_startCoordinateEditor->setValue(hitObject.startX);
        m_layout->addRow(QStringLiteral("Start position"), m_startCoordinateEditor);
        m_startWidthEditor = new QDoubleSpinBox(this);
        m_startWidthEditor->setRange(kMinimumSkyWidth, kMaximumSkyWidth);
        m_startWidthEditor->setDecimals(3);
        m_startWidthEditor->setSingleStep(0.025);
        m_startWidthEditor->setValue(hitObject.startWidth);
        m_layout->addRow(QStringLiteral("Start width"), m_startWidthEditor);
        m_endCoordinateEditor = new QDoubleSpinBox(this);
        m_endCoordinateEditor->setRange(kCoordinateMinimum, kCoordinateMaximum);
        m_endCoordinateEditor->setDecimals(3);
        m_endCoordinateEditor->setSingleStep(0.025);
        m_endCoordinateEditor->setValue(hitObject.endX);
        m_layout->addRow(QStringLiteral("End position"), m_endCoordinateEditor);
        m_endWidthCoordinateEditor = new QDoubleSpinBox(this);
        m_endWidthCoordinateEditor->setRange(kMinimumSkyWidth, kMaximumSkyWidth);
        m_endWidthCoordinateEditor->setDecimals(3);
        m_endWidthCoordinateEditor->setSingleStep(0.025);
        m_endWidthCoordinateEditor->setValue(hitObject.endWidth);
        m_layout->addRow(QStringLiteral("End width"), m_endWidthCoordinateEditor);
    } else {
        m_laneEditor = new QSpinBox(this);
        m_laneEditor->setRange(0, kLastFloorLane);
        m_laneEditor->setValue(floorLane(hitObject.side, hitObject.startX));
        m_layout->addRow(QStringLiteral("First lane"), m_laneEditor);
        m_endLaneEditor = new QSpinBox(this);
        m_endLaneEditor->setRange(0, kLastFloorLane);
        const int lastLane = hitObject.side == FloorSide::Central
            ? std::clamp(floorLane(hitObject.side, hitObject.startX) + static_cast<int>(std::lround(hitObject.startWidth * 4.0)) - 1,
                kFirstCentralLane,
                kLastCentralLane)
            : floorLane(hitObject.side, hitObject.startX);
        m_endLaneEditor->setValue(lastLane);
        m_layout->addRow(QStringLiteral("Last lane"), m_endLaneEditor);
    }
    m_auxiliaryEditor = new QSpinBox(this);
    m_auxiliaryEditor->setRange(0, kMaximumAuxiliaryValue);
    m_auxiliaryEditor->setValue(static_cast<int>(std::min<std::uint32_t>(hitObject.auxiliary, kMaximumAuxiliaryValue)));
    m_layout->addRow(QStringLiteral("Encoding"), m_auxiliaryEditor);

    const auto commit = [this] { commitHitObjectEdit(); };
    connect(m_offsetEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_endEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_auxiliaryEditor, &QSpinBox::editingFinished, this, commit);
    if (isSkyObject) {
        connect(m_startCoordinateEditor, &QDoubleSpinBox::editingFinished, this, commit);
        connect(m_startWidthEditor, &QDoubleSpinBox::editingFinished, this, commit);
        connect(m_endCoordinateEditor, &QDoubleSpinBox::editingFinished, this, commit);
        connect(m_endWidthCoordinateEditor, &QDoubleSpinBox::editingFinished, this, commit);
    } else {
        connect(m_laneEditor, &QSpinBox::editingFinished, this, commit);
        connect(m_endLaneEditor, &QSpinBox::editingFinished, this, commit);
    }
}

void PropertiesPanel::showTimingPoint(const int index, const TimingPoint& timingPoint) {
    constexpr std::array<int, 6> kSignatureDenominators{1, 2, 4, 8, 16, 32};

    m_timingPointIndex = index;
    m_timingPoint = timingPoint;
    addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("Timing point"));
    m_offsetEditor = new QSpinBox(this);
    m_offsetEditor->setRange(0, kMaximumOffsetMilliseconds);
    m_offsetEditor->setSuffix(QStringLiteral(" ms"));
    m_offsetEditor->setValue(static_cast<int>(std::min<qint64>(timingPoint.timeMilliseconds, kMaximumOffsetMilliseconds)));
    m_layout->addRow(QStringLiteral("Offset"), m_offsetEditor);
    m_bpmEditor = new QDoubleSpinBox(this);
    m_bpmEditor->setRange(kMinimumBpm, kMaximumBpm);
    m_bpmEditor->setDecimals(3);
    m_bpmEditor->setSingleStep(0.5);
    m_bpmEditor->setValue(timingPoint.beatsPerMinute);
    m_layout->addRow(QStringLiteral("BPM"), m_bpmEditor);
    m_numeratorEditor = new QSpinBox(this);
    m_numeratorEditor->setRange(kMinimumSignatureNumerator, kMaximumSignatureNumerator);
    m_numeratorEditor->setValue(timingPoint.timeSignatureNumerator);
    m_layout->addRow(QStringLiteral("Numerator"), m_numeratorEditor);
    m_denominatorEditor = new QComboBox(this);
    for (const int denominator : kSignatureDenominators) {
        m_denominatorEditor->addItem(QString::number(denominator), denominator);
    }
    const int denominatorIndex = m_denominatorEditor->findData(timingPoint.timeSignatureDenominator);
    m_denominatorEditor->setCurrentIndex(std::max(0, denominatorIndex));
    m_layout->addRow(QStringLiteral("Denominator"), m_denominatorEditor);

    const auto commit = [this] { commitTimingPointEdit(); };
    connect(m_offsetEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_bpmEditor, &QDoubleSpinBox::editingFinished, this, commit);
    connect(m_numeratorEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_denominatorEditor, &QComboBox::activated, this, [commit](const int) { commit(); });
}

void PropertiesPanel::commitHitObjectEdit() {
    if (m_populating || m_hitObjectIndex < 0 || m_offsetEditor == nullptr || m_endEditor == nullptr) {
        return;
    }

    ChartNote editedHitObject = m_hitObject;
    editedHitObject.startMilliseconds = m_offsetEditor->value();
    editedHitObject.endMilliseconds = m_endEditor->value();
    editedHitObject.auxiliary = static_cast<std::uint32_t>(m_auxiliaryEditor->value());
    if (m_startCoordinateEditor != nullptr) {
        editedHitObject.startX = m_startCoordinateEditor->value();
        editedHitObject.startWidth = m_startWidthEditor->value();
        editedHitObject.endX = m_endCoordinateEditor->value();
        editedHitObject.endWidth = m_endWidthCoordinateEditor->value();
    } else {
        setFloorSpan(editedHitObject, m_laneEditor->value(), m_endLaneEditor->value());
    }
    m_hitObject = editedHitObject;
    emit hitObjectEditRequested(m_hitObjectIndex, editedHitObject);
}

void PropertiesPanel::commitTimingPointEdit() {
    if (m_populating || m_timingPointIndex < 0 || m_offsetEditor == nullptr || m_bpmEditor == nullptr) {
        return;
    }

    TimingPoint editedTimingPoint = m_timingPoint;
    editedTimingPoint.timeMilliseconds = m_offsetEditor->value();
    editedTimingPoint.beatsPerMinute = m_bpmEditor->value();
    editedTimingPoint.timeSignatureNumerator = m_numeratorEditor->value();
    editedTimingPoint.timeSignatureDenominator = m_denominatorEditor->currentData().toInt();
    m_timingPoint = editedTimingPoint;
    emit timingPointEditRequested(m_timingPointIndex, editedTimingPoint);
}

void PropertiesPanel::showLaneEvent(const int index, const LaneEvent& laneEvent) {
    m_laneEventIndex = index;
    m_laneEvent = laneEvent;
    addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("Lane event"));
    m_offsetEditor = new QSpinBox(this);
    m_offsetEditor->setRange(0, kMaximumOffsetMilliseconds);
    m_offsetEditor->setSuffix(QStringLiteral(" ms"));
    m_offsetEditor->setValue(static_cast<int>(std::min<qint64>(laneEvent.timeMilliseconds, kMaximumOffsetMilliseconds)));
    m_layout->addRow(QStringLiteral("Time"), m_offsetEditor);
    m_laneEditor = new QSpinBox(this);
    m_laneEditor->setRange(0, 5);
    m_laneEditor->setValue(laneEvent.lane);
    m_layout->addRow(QStringLiteral("Lane"), m_laneEditor);
    m_laneEventStateEditor = new QComboBox(this);
    m_laneEventStateEditor->addItem(QStringLiteral("OFF (dim)"), false);
    m_laneEventStateEditor->addItem(QStringLiteral("ON (restore)"), true);
    m_laneEventStateEditor->setCurrentIndex(laneEvent.enabled ? 1 : 0);
    m_layout->addRow(QStringLiteral("State"), m_laneEventStateEditor);
    const auto commit = [this] { commitLaneEventEdit(); };
    connect(m_offsetEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_laneEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_laneEventStateEditor, &QComboBox::activated, this, [commit](const int) { commit(); });
}

void PropertiesPanel::commitLaneEventEdit() {
    if (m_populating || m_laneEventIndex < 0 || m_offsetEditor == nullptr
        || m_laneEditor == nullptr || m_laneEventStateEditor == nullptr) {
        return;
    }
    LaneEvent editedLaneEvent = m_laneEvent;
    editedLaneEvent.timeMilliseconds = m_offsetEditor->value();
    editedLaneEvent.lane = m_laneEditor->value();
    editedLaneEvent.enabled = m_laneEventStateEditor->currentData().toBool();
    m_laneEvent = editedLaneEvent;
    emit laneEventEditRequested(m_laneEventIndex, editedLaneEvent);
}

void PropertiesPanel::showSpeedEvent(const int index, const SpeedEvent& speedEvent) {
    m_speedEventIndex = index;
    m_speedEvent = speedEvent;
    addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("SV event"));
    m_offsetEditor = new QSpinBox(this);
    m_offsetEditor->setRange(0, kMaximumOffsetMilliseconds);
    m_offsetEditor->setSuffix(QStringLiteral(" ms"));
    m_offsetEditor->setValue(static_cast<int>(std::min<qint64>(speedEvent.timeMilliseconds, kMaximumOffsetMilliseconds)));
    m_layout->addRow(QStringLiteral("Time"), m_offsetEditor);
    m_speedEditor = new QDoubleSpinBox(this);
    m_speedEditor->setRange(-100.0, 100.0);
    m_speedEditor->setDecimals(3);
    m_speedEditor->setSingleStep(0.05);
    m_speedEditor->setSuffix(QStringLiteral("x"));
    m_speedEditor->setValue(speedEvent.speed);
    m_layout->addRow(QStringLiteral("Speed"), m_speedEditor);
    const auto commit = [this] { commitSpeedEventEdit(); };
    connect(m_offsetEditor, &QSpinBox::editingFinished, this, commit);
    connect(m_speedEditor, &QDoubleSpinBox::editingFinished, this, commit);
}

void PropertiesPanel::commitSpeedEventEdit() {
    if (m_populating || m_speedEventIndex < 0 || m_offsetEditor == nullptr || m_speedEditor == nullptr) {
        return;
    }
    SpeedEvent editedSpeedEvent = m_speedEvent;
    editedSpeedEvent.timeMilliseconds = m_offsetEditor->value();
    editedSpeedEvent.speed = m_speedEditor->value();
    m_speedEvent = editedSpeedEvent;
    emit speedEventEditRequested(m_speedEventIndex, editedSpeedEvent);
}

} // namespace infalsus::gui
