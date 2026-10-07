#include "gui/widgets/properties_panel.h"

#include <QtCore/QVariant>
#include <QtGui/QValidator>
#include <QtWidgets/QComboBox>
#include <QtWidgets/QDoubleSpinBox>
#include <QtWidgets/QFormLayout>
#include <QtWidgets/QLabel>
#include <QtWidgets/QLineEdit>
#include <QtWidgets/QSpinBox>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>

namespace infalsus::gui {

namespace {

constexpr int kMaximumOffsetMilliseconds = 36'000'000;
constexpr int kMaximumAuxiliaryValue = std::numeric_limits<int>::max();
constexpr int kFirstCentralLane = 1;
constexpr int kLastCentralLane = 4;
constexpr int kLastFloorLane = 5;
constexpr int kCoordinateDecimals = 3;
constexpr double kCoordinateMinimum = 0.0;
constexpr double kCoordinateMaximum = 1.0;
constexpr double kCoordinateStep = 0.025;
constexpr double kMinimumSkyWidth = 0.01;
constexpr double kMaximumSkyWidth = 1.0;
constexpr double kMinimumBpm = 1.0;
constexpr double kMaximumBpm = 1000.0;
constexpr double kBpmStep = 0.5;
constexpr double kMinimumSpeed = -100.0;
constexpr double kMaximumSpeed = 100.0;
constexpr double kSpeedStep = 0.05;
constexpr int kMinimumSignatureNumerator = 1;
constexpr int kMaximumSignatureNumerator = 32;

class GroupIdValidator final : public QValidator {
public:
    using QValidator::QValidator;

    State validate(QString& input, int& position) const override {
        Q_UNUSED(position);
        if (input.isEmpty()) {
            return Intermediate;
        }
        if (!std::all_of(input.cbegin(), input.cend(), [](const QChar character) {
            return character >= QLatin1Char('0') && character <= QLatin1Char('9');
        })) {
            return Invalid;
        }
        bool valid = false;
        input.toULongLong(&valid);

        return valid ? Acceptable : Invalid;
    }
};

template <typename Value>
[[nodiscard]] bool hasMixedValues(const QVector<Value>& values) {
    return std::any_of(values.cbegin(), values.cend(), [&values](const Value& value) {
        return value != values.front();
    });
}

void setMixedStyle(QWidget* editor, const bool mixed) {
    editor->setProperty("mixed", mixed);
    editor->setStyleSheet(mixed ? QStringLiteral("color: #888;") : QString());
    editor->setToolTip(mixed ? QStringLiteral("Mixed values. Editing overrides this property for the selection.") : QString());
}

template <typename Object, typename Getter>
[[nodiscard]] auto propertyValues(const QVector<Object>& objects, Getter getter) {
    QVector<std::decay_t<decltype(getter(objects.front()))>> values;
    values.reserve(objects.size());
    for (const Object& object : objects) {
        values.append(getter(object));
    }

    return values;
}

template <typename SpinBox, typename Value, typename Apply>
void addNumberField(QFormLayout* layout, const QString& label, const QVector<Value>& values,
    const Value minimum, const Value maximum, const Value step, const QString& suffix, Apply apply) {
    auto* editor = new SpinBox(layout->parentWidget());
    const double average = std::accumulate(values.cbegin(), values.cend(), 0.0) / values.size();

    editor->setObjectName(label);
    editor->setRange(minimum, maximum);
    editor->setSingleStep(step);
    editor->setSuffix(suffix);
    editor->setKeyboardTracking(false);
    if constexpr (std::is_same_v<SpinBox, QDoubleSpinBox>) {
        editor->setDecimals(kCoordinateDecimals);
        editor->setValue(average);
    } else {
        editor->setValue(static_cast<int>(std::lround(average)));
    }
    setMixedStyle(editor, hasMixedValues(values));
    layout->addRow(label, editor);
    const auto markEdited = [editor] { editor->setProperty("edited", true); };
    QObject::connect(editor, &SpinBox::valueChanged, editor, markEdited);
    QObject::connect(editor->template findChild<QLineEdit*>(), &QLineEdit::textEdited, editor, markEdited);
    QObject::connect(editor, &SpinBox::editingFinished, editor, [editor, apply] {
        if (!editor->property("edited").toBool() || !editor->hasAcceptableInput()) {
            return;
        }
        editor->setProperty("edited", false);
        setMixedStyle(editor, false);
        apply(editor->value());
    });
}

template <typename Apply>
void addChoiceField(QFormLayout* layout, const QString& label, const QVector<int>& values,
    const QVector<QPair<QString, int>>& choices, Apply apply) {
    auto* editor = new QComboBox(layout->parentWidget());
    const bool mixed = hasMixedValues(values);

    editor->setObjectName(label);
    editor->setPlaceholderText(QStringLiteral("Mixed"));
    for (const auto& [text, value] : choices) {
        editor->addItem(text, value);
    }
    if (!mixed && editor->findData(values.front()) < 0) {
        editor->addItem(QString::number(values.front()), values.front());
    }
    editor->setCurrentIndex(mixed ? -1 : editor->findData(values.front()));
    setMixedStyle(editor, mixed);
    layout->addRow(label, editor);
    QObject::connect(editor, &QComboBox::activated, editor, [editor, apply](const int index) {
        setMixedStyle(editor, false);
        apply(editor->itemData(index).toInt());
    });
}

[[nodiscard]] QString hitObjectName(const ChartNote& hitObject) {
    switch (hitObject.kind) {
    case NoteKind::Tap: return QStringLiteral("Note");
    case NoteKind::Hold: return QStringLiteral("Hold");
    case NoteKind::Flick: return QStringLiteral("Flick");
    case NoteKind::Sky: return QStringLiteral("Zone");
    case NoteKind::Unknown: return QStringLiteral("Unknown");
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

    return std::clamp(static_cast<int>(std::lround(coordinate * kLastCentralLane)), kFirstCentralLane, kLastCentralLane);
}

[[nodiscard]] int lastFloorLane(const ChartNote& hitObject) {
    const int firstLane = floorLane(hitObject.side, hitObject.startX);

    return hitObject.side == FloorSide::Central
        ? std::clamp(firstLane + static_cast<int>(std::lround(hitObject.startWidth * kLastCentralLane)) - 1,
            kFirstCentralLane, kLastCentralLane)
        : firstLane;
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
    const int clampedLastLane = std::clamp(lastLane, clampedFirstLane, kLastCentralLane);
    const int laneCount = clampedLastLane - clampedFirstLane + 1;
    const double coordinate = static_cast<double>(clampedFirstLane) / kLastCentralLane;
    const double normalizedWidth = static_cast<double>(laneCount) / kLastCentralLane;
    hitObject.side = FloorSide::Central;
    hitObject.startX = coordinate;
    hitObject.endX = coordinate;
    hitObject.startWidth = normalizedWidth;
    hitObject.endWidth = normalizedWidth;
}

[[nodiscard]] int displayedTime(const qint64 time) {
    return static_cast<int>(std::clamp<qint64>(time, 0, kMaximumOffsetMilliseconds));
}

} // namespace

PropertiesPanel::PropertiesPanel(QWidget* parent)
    : QWidget(parent) {
    m_layout = new QFormLayout(this);
    addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("No selection"));
}

void PropertiesPanel::setSelection(const EditorState& state) {
    m_populating = true;
    clearRows();
    m_hitObjectIndexes = state.selectedHitObjects();
    for (const int index : m_hitObjectIndexes) {
        m_hitObjects.append(state.chart().notes.at(index));
    }
    for (const int index : state.selectedTimingPoints()) {
        m_timingPoints.append(state.timingPoints().at(index));
    }
    for (const int index : state.selectedLaneEvents()) {
        m_laneEvents.append(state.laneEvents().at(index));
    }
    for (const int index : state.selectedSpeedEvents()) {
        m_speedEvents.append(state.speedEvents().at(index));
    }
    if (!m_hitObjects.isEmpty()) {
        showHitObjects();
    } else if (!m_timingPoints.isEmpty() || !m_laneEvents.isEmpty() || !m_speedEvents.isEmpty()) {
        showEvents();
    } else {
        addReadOnlyRow(QStringLiteral("Selection"), QStringLiteral("No selection"));
    }
    m_populating = false;
}

void PropertiesPanel::clearRows() {
    while (m_layout->rowCount() > 0) {
        m_layout->removeRow(0);
    }
    m_hitObjectIndexes.clear();
    m_hitObjects.clear();
    m_timingPoints.clear();
    m_laneEvents.clear();
    m_speedEvents.clear();
}

void PropertiesPanel::addReadOnlyRow(const QString& label, const QString& value) {
    auto* valueLabel = new QLabel(value, this);
    valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_layout->addRow(label, valueLabel);
}

void PropertiesPanel::showHitObjects() {
    const QVector<QString> types = propertyValues(m_hitObjects, hitObjectName);
    QVector<ChartNote> zones;
    QVector<ChartNote> skyObjects;
    QVector<ChartNote> floorObjects;

    addReadOnlyRow(QStringLiteral("Type"), hasMixedValues(types) ? QStringLiteral("Mixed") : types.front());
    if (m_hitObjects.size() == 1) {
        addReadOnlyRow(QStringLiteral("ID"), QString::number(m_hitObjects.front().id));
    }
    for (const ChartNote& hitObject : m_hitObjects) {
        if (hitObject.kind == NoteKind::Sky) {
            zones.append(hitObject);
        }
        (hitObject.side == FloorSide::Sky ? skyObjects : floorObjects).append(hitObject);
    }
    if (!zones.isEmpty()) {
        const auto values = propertyValues(zones, [](const ChartNote& hitObject) { return hitObject.groupId; });
        auto* editor = new QLineEdit(this);
        const bool mixed = hasMixedValues(values);

        editor->setObjectName(QStringLiteral("Group ID"));
        editor->setValidator(new GroupIdValidator(editor));
        editor->setPlaceholderText(QStringLiteral("Mixed"));
        editor->setText(mixed ? QString() : QString::number(values.front()));
        setMixedStyle(editor, mixed);
        m_layout->addRow(QStringLiteral("Group ID"), editor);
        connect(editor, &QLineEdit::textEdited, editor, [editor] { editor->setProperty("edited", true); });
        connect(editor, &QLineEdit::editingFinished, editor, [this, editor] {
            if (!editor->property("edited").toBool() || !editor->hasAcceptableInput()) {
                return;
            }
            editor->setProperty("edited", false);
            setMixedStyle(editor, false);
            applyHitObjectProperty(HitObjectProperty::GroupId, editor->text().toULongLong());
        });
    }
    const auto apply = [this](const HitObjectProperty property) {
        return [this, property](const auto value) { applyHitObjectProperty(property, QVariant(value)); };
    };
    const auto addTime = [&](const QString& label, const HitObjectProperty property, auto getter) {
        addNumberField<QSpinBox>(m_layout, label, propertyValues(m_hitObjects, getter),
            0, kMaximumOffsetMilliseconds, 1, QStringLiteral(" ms"), apply(property));
    };
    addTime(QStringLiteral("Offset"), HitObjectProperty::Offset,
        [](const ChartNote& hitObject) { return displayedTime(hitObject.startMilliseconds); });
    addTime(QStringLiteral("End"), HitObjectProperty::End,
        [](const ChartNote& hitObject) { return displayedTime(hitObject.endMilliseconds); });
    if (!skyObjects.isEmpty()) {
        const auto addCoordinate = [&](const QString& label, const HitObjectProperty property,
            auto getter, const bool width) {
            addNumberField<QDoubleSpinBox>(m_layout, label, propertyValues(skyObjects, getter),
                width ? kMinimumSkyWidth : kCoordinateMinimum, width ? kMaximumSkyWidth : kCoordinateMaximum,
                kCoordinateStep, QString(), apply(property));
        };
        addCoordinate(QStringLiteral("Start position"), HitObjectProperty::StartPosition,
            [](const ChartNote& hitObject) { return hitObject.startX; }, false);
        addCoordinate(QStringLiteral("Start width"), HitObjectProperty::StartWidth,
            [](const ChartNote& hitObject) { return hitObject.startWidth; }, true);
        addCoordinate(QStringLiteral("End position"), HitObjectProperty::EndPosition,
            [](const ChartNote& hitObject) { return hitObject.endX; }, false);
        addCoordinate(QStringLiteral("End width"), HitObjectProperty::EndWidth,
            [](const ChartNote& hitObject) { return hitObject.endWidth; }, true);
    }
    if (!floorObjects.isEmpty()) {
        addNumberField<QSpinBox>(m_layout, QStringLiteral("First lane"), propertyValues(floorObjects,
            [](const ChartNote& hitObject) { return floorLane(hitObject.side, hitObject.startX); }),
            0, kLastFloorLane, 1, QString(), apply(HitObjectProperty::FirstLane));
        addNumberField<QSpinBox>(m_layout, QStringLiteral("Last lane"), propertyValues(floorObjects, lastFloorLane),
            0, kLastFloorLane, 1, QString(), apply(HitObjectProperty::LastLane));
    }
    addNumberField<QSpinBox>(m_layout, QStringLiteral("Encoding"), propertyValues(m_hitObjects,
        [](const ChartNote& hitObject) {
            return static_cast<int>(std::min<std::uint32_t>(hitObject.auxiliary, kMaximumAuxiliaryValue));
        }), 0, kMaximumAuxiliaryValue, 1, QString(), apply(HitObjectProperty::Encoding));
}

void PropertiesPanel::showEvents() {
    QVector<int> times;
    QVector<QString> types;
    const auto apply = [this](const EventProperty property) {
        return [this, property](const auto value) { applyEventProperty(property, QVariant(value)); };
    };
    const auto collectTimes = [&](const auto& events, const QString& type) {
        if (!events.isEmpty()) {
            times += propertyValues(events, [](const auto& event) { return displayedTime(event.timeMilliseconds); });
            types.append(type);
        }
    };

    collectTimes(m_timingPoints, QStringLiteral("Timing point"));
    collectTimes(m_laneEvents, QStringLiteral("Lane event"));
    collectTimes(m_speedEvents, QStringLiteral("SV event"));
    addReadOnlyRow(QStringLiteral("Type"), types.size() == 1 ? types.front() : QStringLiteral("Mixed"));
    addNumberField<QSpinBox>(m_layout, QStringLiteral("Time"), times,
        0, kMaximumOffsetMilliseconds, 1, QStringLiteral(" ms"), apply(EventProperty::Time));
    if (!m_timingPoints.isEmpty()) {
        addNumberField<QDoubleSpinBox>(m_layout, QStringLiteral("BPM"), propertyValues(m_timingPoints,
            [](const TimingPoint& point) { return point.beatsPerMinute; }),
            kMinimumBpm, kMaximumBpm, kBpmStep, QString(), apply(EventProperty::Bpm));
        addNumberField<QSpinBox>(m_layout, QStringLiteral("Numerator"), propertyValues(m_timingPoints,
            [](const TimingPoint& point) { return point.timeSignatureNumerator; }),
            kMinimumSignatureNumerator, kMaximumSignatureNumerator, 1, QString(), apply(EventProperty::Numerator));
        const QVector<QPair<QString, int>> denominators{{QStringLiteral("1"), 1}, {QStringLiteral("2"), 2},
            {QStringLiteral("4"), 4}, {QStringLiteral("8"), 8}, {QStringLiteral("16"), 16},
            {QStringLiteral("32"), 32}, {QStringLiteral("64"), 64}};
        addChoiceField(m_layout, QStringLiteral("Denominator"), propertyValues(m_timingPoints,
            [](const TimingPoint& point) { return point.timeSignatureDenominator; }),
            denominators, apply(EventProperty::Denominator));
    }
    if (!m_laneEvents.isEmpty()) {
        addNumberField<QSpinBox>(m_layout, QStringLiteral("Lane"), propertyValues(m_laneEvents,
            [](const LaneEvent& event) { return event.lane; }),
            0, kLastFloorLane, 1, QString(), apply(EventProperty::Lane));
        const QVector<QPair<QString, int>> states{{QStringLiteral("OFF (dim)"), 0}, {QStringLiteral("ON (restore)"), 1}};
        addChoiceField(m_layout, QStringLiteral("State"), propertyValues(m_laneEvents,
            [](const LaneEvent& event) { return event.enabled ? 1 : 0; }), states, apply(EventProperty::Enabled));
    }
    if (!m_speedEvents.isEmpty()) {
        addNumberField<QDoubleSpinBox>(m_layout, QStringLiteral("Speed"), propertyValues(m_speedEvents,
            [](const SpeedEvent& event) { return event.speed; }),
            kMinimumSpeed, kMaximumSpeed, kSpeedStep, QStringLiteral("x"), apply(EventProperty::Speed));
    }
}

void PropertiesPanel::applyHitObjectProperty(const HitObjectProperty property, const QVariant& value) {
    if (m_populating) {
        return;
    }
    for (ChartNote& hitObject : m_hitObjects) {
        const bool sky = hitObject.side == FloorSide::Sky;
        switch (property) {
        case HitObjectProperty::GroupId:
            if (hitObject.kind == NoteKind::Sky) { hitObject.groupId = value.toULongLong(); }
            break;
        case HitObjectProperty::Offset: hitObject.startMilliseconds = value.toInt(); break;
        case HitObjectProperty::End: hitObject.endMilliseconds = value.toInt(); break;
        case HitObjectProperty::Encoding: hitObject.auxiliary = value.toUInt(); break;
        case HitObjectProperty::FirstLane:
            if (!sky) { setFloorSpan(hitObject, value.toInt(), lastFloorLane(hitObject)); }
            break;
        case HitObjectProperty::LastLane:
            if (!sky) { setFloorSpan(hitObject, floorLane(hitObject.side, hitObject.startX), value.toInt()); }
            break;
        case HitObjectProperty::StartPosition:
            if (sky) { hitObject.startX = value.toDouble(); }
            break;
        case HitObjectProperty::StartWidth:
            if (sky) { hitObject.startWidth = value.toDouble(); }
            break;
        case HitObjectProperty::EndPosition:
            if (sky) { hitObject.endX = value.toDouble(); }
            break;
        case HitObjectProperty::EndWidth:
            if (sky) { hitObject.endWidth = value.toDouble(); }
            break;
        }
    }
    emit hitObjectsEditRequested(m_hitObjectIndexes, m_hitObjects);
}

void PropertiesPanel::applyEventProperty(const EventProperty property, const QVariant& value) {
    if (m_populating) {
        return;
    }
    for (TimingPoint& point : m_timingPoints) {
        switch (property) {
        case EventProperty::Time: point.timeMilliseconds = value.toInt(); break;
        case EventProperty::Bpm: point.beatsPerMinute = value.toDouble(); break;
        case EventProperty::Numerator: point.timeSignatureNumerator = value.toInt(); break;
        case EventProperty::Denominator: point.timeSignatureDenominator = value.toInt(); break;
        default: break;
        }
    }
    for (LaneEvent& event : m_laneEvents) {
        switch (property) {
        case EventProperty::Time: event.timeMilliseconds = value.toInt(); break;
        case EventProperty::Lane: event.lane = value.toInt(); break;
        case EventProperty::Enabled: event.enabled = value.toBool(); break;
        default: break;
        }
    }
    for (SpeedEvent& event : m_speedEvents) {
        switch (property) {
        case EventProperty::Time: event.timeMilliseconds = value.toInt(); break;
        case EventProperty::Speed: event.speed = value.toDouble(); break;
        default: break;
        }
    }
    emit eventsEditRequested(m_timingPoints, m_laneEvents, m_speedEvents);
}

} // namespace infalsus::gui
