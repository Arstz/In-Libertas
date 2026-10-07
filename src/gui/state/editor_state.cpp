#include "gui/state/editor_state.h"

#include "core/timing_grid.h"

#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numeric>
#include <type_traits>

namespace infalsus::gui {

namespace {

constexpr std::array<int, 12> kDivisors{1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};
constexpr std::uint32_t kZoneLeftEasingMask = 0x1CU;
constexpr std::uint32_t kZoneRightEasingMask = 0xE0U;
constexpr std::uint32_t kZoneLeftSineOut = 0x08U;
constexpr std::uint32_t kZoneLeftSineIn = 0x10U;
constexpr std::uint32_t kZoneRightSineOut = 0x40U;
constexpr std::uint32_t kZoneRightSineIn = 0x80U;
constexpr std::uint32_t kFlickRight = 0x400U;
constexpr std::uint32_t kFlickLeft = 0x1000U;
constexpr qint64 kMinimumZoneDurationMilliseconds = 1;

[[nodiscard]] bool isSkyHitObject(const ChartNote& hitObject) {
    return hitObject.kind == NoteKind::Sky || hitObject.kind == NoteKind::Flick;
}

void constrainZoneDuration(ChartNote& hitObject) {
    if (hitObject.kind == NoteKind::Sky) {
        hitObject.endMilliseconds = std::max(
            hitObject.endMilliseconds,
            hitObject.startMilliseconds + kMinimumZoneDurationMilliseconds);
    }
}

void normalizeHitObject(ChartNote& hitObject) {
    hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds);
    hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, hitObject.endMilliseconds);
    constrainZoneDuration(hitObject);
    hitObject.startX = std::clamp(hitObject.startX, 0.0, 1.0);
    hitObject.endX = std::clamp(hitObject.endX, 0.0, 1.0);
    hitObject.startWidth = std::clamp(hitObject.startWidth, 0.01, 1.0);
    hitObject.endWidth = std::clamp(hitObject.endWidth, 0.01, 1.0);
    if (hitObject.side == FloorSide::Central) {
        constexpr double kLaneWidth = 0.25;
        constexpr double kFirstCentralLane = 0.25;
        constexpr double kLastCentralLane = 1.0;
        constexpr double kCentralRightBoundary = 1.25;
        hitObject.startX = std::clamp(hitObject.startX, kFirstCentralLane, kLastCentralLane);
        hitObject.endX = std::clamp(hitObject.endX, kFirstCentralLane, kLastCentralLane);
        hitObject.startWidth = std::clamp(hitObject.startWidth, kLaneWidth, kCentralRightBoundary - hitObject.startX);
        hitObject.endWidth = std::clamp(hitObject.endWidth, kLaneWidth, kCentralRightBoundary - hitObject.endX);
    }
}

void normalizeEvent(TimingPoint& point) {
    constexpr double kMinimumBpm = 1.0;
    constexpr double kMaximumBpm = 1000.0;
    constexpr int kMinimumSignaturePart = 1;
    constexpr int kMaximumNumerator = 32;
    constexpr int kMaximumDenominator = 64;
    point.timeMilliseconds = std::max<qint64>(0, point.timeMilliseconds);
    point.beatsPerMinute = std::clamp(point.beatsPerMinute, kMinimumBpm, kMaximumBpm);
    point.timeSignatureNumerator = std::clamp(point.timeSignatureNumerator, kMinimumSignaturePart, kMaximumNumerator);
    point.timeSignatureDenominator = std::clamp(point.timeSignatureDenominator, kMinimumSignaturePart, kMaximumDenominator);
}

void normalizeEvent(LaneEvent& event) {
    constexpr int kLastLane = 5;
    event.timeMilliseconds = std::max<qint64>(0, event.timeMilliseconds);
    event.lane = std::clamp(event.lane, 0, kLastLane);
}

void normalizeEvent(SpeedEvent& event) {
    event.timeMilliseconds = std::max<qint64>(0, event.timeMilliseconds);
}

[[nodiscard]] bool eventsEqual(const TimingPoint& first, const TimingPoint& second) {
    return first.timeMilliseconds == second.timeMilliseconds && first.beatsPerMinute == second.beatsPerMinute
        && first.timeSignatureNumerator == second.timeSignatureNumerator
        && first.timeSignatureDenominator == second.timeSignatureDenominator;
}

[[nodiscard]] bool eventsEqual(const LaneEvent& first, const LaneEvent& second) {
    return first.timeMilliseconds == second.timeMilliseconds && first.lane == second.lane && first.enabled == second.enabled;
}

[[nodiscard]] bool eventsEqual(const SpeedEvent& first, const SpeedEvent& second) {
    return first.timeMilliseconds == second.timeMilliseconds && first.speed == second.speed;
}

template <typename Event>
[[nodiscard]] bool replaceSelectedEvents(QVector<Event>& events, QVector<int>& selection, QVector<Event> replacements) {
    QVector<int> order(events.size());
    QVector<int> sortedSelection;
    QVector<Event> sortedEvents;
    bool changed = false;

    for (int itemIndex = 0; itemIndex < selection.size(); ++itemIndex) {
        Event& replacement = replacements[itemIndex];
        normalizeEvent(replacement);
        changed = changed || !eventsEqual(events.at(selection.at(itemIndex)), replacement);
    }
    if (!changed) {
        return false;
    }
    for (int itemIndex = 0; itemIndex < selection.size(); ++itemIndex) {
        events[selection.at(itemIndex)] = replacements.at(itemIndex);
    }
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&events](const int first, const int second) {
        return events.at(first).timeMilliseconds < events.at(second).timeMilliseconds;
    });
    sortedEvents.reserve(events.size());
    for (const int originalIndex : order) {
        if (std::binary_search(selection.cbegin(), selection.cend(), originalIndex)) {
            sortedSelection.append(static_cast<int>(sortedEvents.size()));
        }
        sortedEvents.append(events.at(originalIndex));
    }
    events = std::move(sortedEvents);
    selection = std::move(sortedSelection);

    return true;
}

void mirrorGroundHitObject(ChartNote& hitObject) {
    if (hitObject.side == FloorSide::LeftOuter || hitObject.side == FloorSide::RightOuter) {
        hitObject.side = hitObject.side == FloorSide::LeftOuter ? FloorSide::RightOuter : FloorSide::LeftOuter;
        hitObject.startX = 0.0;
        hitObject.endX = 0.0;
        hitObject.startWidth = 1.0;
        hitObject.endWidth = 1.0;
        return;
    }

    constexpr double kCentralMirrorExtent = 1.5;
    hitObject.startX = kCentralMirrorExtent - hitObject.startX - hitObject.startWidth;
    hitObject.endX = kCentralMirrorExtent - hitObject.endX - hitObject.endWidth;
}

void mirrorZoneEasing(ChartNote& hitObject) {
    const std::uint32_t otherFlags = hitObject.auxiliary & ~(kZoneLeftEasingMask | kZoneRightEasingMask);
    const std::uint32_t leftEasing = hitObject.auxiliary & kZoneLeftEasingMask;
    const std::uint32_t rightEasing = hitObject.auxiliary & kZoneRightEasingMask;

    hitObject.auxiliary = otherFlags | (leftEasing << 3) | (rightEasing >> 3);
}

void mirrorFlickDirection(ChartNote& hitObject) {
    const std::uint32_t otherFlags = hitObject.auxiliary & ~(kFlickRight | kFlickLeft);
    const bool pointsRight = (hitObject.auxiliary & kFlickRight) != 0
        || (hitObject.auxiliary & kFlickLeft) == 0;

    hitObject.auxiliary = otherFlags | (pointsRight ? kFlickLeft : kFlickRight);
}

void reverseZoneEasing(ChartNote& hitObject) {
    const std::uint32_t otherFlags = hitObject.auxiliary & ~(kZoneLeftEasingMask | kZoneRightEasingMask);
    const std::uint32_t leftEasing = hitObject.auxiliary & kZoneLeftEasingMask;
    const std::uint32_t rightEasing = hitObject.auxiliary & kZoneRightEasingMask;
    const std::uint32_t reversedLeftEasing = (leftEasing & ~(kZoneLeftSineOut | kZoneLeftSineIn))
        | ((leftEasing & kZoneLeftSineOut) << 1)
        | ((leftEasing & kZoneLeftSineIn) >> 1);
    const std::uint32_t reversedRightEasing = (rightEasing & ~(kZoneRightSineOut | kZoneRightSineIn))
        | ((rightEasing & kZoneRightSineOut) << 1)
        | ((rightEasing & kZoneRightSineIn) >> 1);

    hitObject.auxiliary = otherFlags | reversedLeftEasing | reversedRightEasing;
}

void flipHitObjectVertically(ChartNote& hitObject, const qint64 selectionStartMilliseconds,
    const qint64 selectionEndMilliseconds) {
    const qint64 startMilliseconds = hitObject.startMilliseconds;
    const qint64 endMilliseconds = hitObject.endMilliseconds;

    hitObject.startMilliseconds = selectionStartMilliseconds + selectionEndMilliseconds - endMilliseconds;
    hitObject.endMilliseconds = selectionStartMilliseconds + selectionEndMilliseconds - startMilliseconds;
    std::swap(hitObject.startX, hitObject.endX);
    std::swap(hitObject.startWidth, hitObject.endWidth);
    if (hitObject.kind == NoteKind::Sky) {
        reverseZoneEasing(hitObject);
    }
}

} // namespace

EditorState::EditorState(QObject* parent)
    : QObject(parent) {
    m_timingPoints.append(TimingPoint{});
    m_speedEvents.append(SpeedEvent{});
}

const ChartData& EditorState::chart() const {
    return m_chart;
}

const ChartMetadata& EditorState::metadata() const {
    return m_metadata;
}

const QVector<TimingPoint>& EditorState::timingPoints() const {
    return m_timingPoints;
}

const QVector<LaneEvent>& EditorState::laneEvents() const {
    return m_laneEvents;
}

const QVector<SpeedEvent>& EditorState::speedEvents() const {
    return m_speedEvents;
}

qint64 EditorState::playbackPosition() const {
    return m_playbackPositionMilliseconds;
}

int EditorState::divisor() const {
    return m_divisor;
}

qint64 EditorState::divisorDurationMilliseconds() const {
    const TimingPoint& point = timingPointAt(m_playbackPositionMilliseconds);
    const double beatDurationMilliseconds = 60000.0 / point.beatsPerMinute;
    const double measureDurationMilliseconds = beatDurationMilliseconds * point.timeSignatureNumerator * 4.0
        / std::max(point.timeSignatureDenominator, 1);

    return std::max<qint64>(1, static_cast<qint64>(std::llround(
        measureDurationMilliseconds / m_divisor)));
}

Difficulty EditorState::difficulty() const {
    return m_difficulty;
}

EditorTool EditorState::tool() const {
    return m_tool;
}

FlatViewMode EditorState::flatViewMode() const {
    return m_flatViewMode;
}

const QVector<int>& EditorState::selectedHitObjects() const {
    return m_selectedHitObjects;
}

int EditorState::selectedTimingPoint() const {
    return m_selectedTimingPoint;
}

const QVector<int>& EditorState::selectedTimingPoints() const {
    return m_selectedTimingPoints;
}

int EditorState::selectedLaneEvent() const {
    return m_selectedLaneEvent;
}

const QVector<int>& EditorState::selectedLaneEvents() const {
    return m_selectedLaneEvents;
}

int EditorState::selectedSpeedEvent() const {
    return m_selectedSpeedEvent;
}

const QVector<int>& EditorState::selectedSpeedEvents() const {
    return m_selectedSpeedEvents;
}

int EditorState::selectedEventCount() const {
    return m_selectedTimingPoints.size() + m_selectedLaneEvents.size() + m_selectedSpeedEvents.size();
}

void EditorState::setChart(ChartData chart) {
    m_chart = std::move(chart);
    m_playbackPositionMilliseconds = std::clamp(m_playbackPositionMilliseconds, qint64(0), m_chart.durationMilliseconds);
    m_undoHistory.clear();
    m_redoHistory.clear();
    m_activeMoveIndex = -1;
    m_activeBatchMoveIndexes.clear();
    m_batchMoveStarts.clear();
    m_moveSelectionStart.clear();
    m_batchMoveSelectionStart.clear();
    m_pendingAddedHitObjectIndex = -1;
    m_activeMoveMergesAdd = false;
    m_selectedHitObjects.clear();
    m_selectedTimingPoint = -1;
    m_selectedTimingPoints.clear();
    m_selectedLaneEvent = -1;
    m_selectedSpeedEvent = -1;
    m_selectedLaneEvents.clear();
    m_selectedSpeedEvents.clear();
    m_nextHitObjectId = 1;
    for (const ChartNote& hitObject : m_chart.notes) {
        m_nextHitObjectId = std::max(m_nextHitObjectId, hitObject.id + 1);
    }
    emit chartChanged();
    emit playbackPositionChanged(m_playbackPositionMilliseconds);
    emit historyChanged(false, false);
    emit selectionChanged();
}

void EditorState::setMetadata(ChartMetadata metadata) {
    m_metadata = std::move(metadata);
    emit metadataChanged();
}

void EditorState::setTimingPoints(QVector<TimingPoint> timingPoints) {
    if (timingPoints.isEmpty()) {
        timingPoints.append(TimingPoint{});
    }
    std::stable_sort(timingPoints.begin(), timingPoints.end(), [](const TimingPoint& first, const TimingPoint& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_timingPoints = std::move(timingPoints);
    emit timingPointsChanged();
    emit divisorChanged(m_divisor);
}

void EditorState::setLaneEvents(QVector<LaneEvent> laneEvents) {
    for (LaneEvent& event : laneEvents) {
        event.timeMilliseconds = std::max<qint64>(0, event.timeMilliseconds);
        event.lane = std::clamp(event.lane, 0, 5);
    }
    std::stable_sort(laneEvents.begin(), laneEvents.end(), [](const LaneEvent& first, const LaneEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_laneEvents = std::move(laneEvents);
    emit laneEventsChanged();
}

void EditorState::setSpeedEvents(QVector<SpeedEvent> speedEvents) {
    for (SpeedEvent& event : speedEvents) {
        event.timeMilliseconds = std::max<qint64>(0, event.timeMilliseconds);
        if (!std::isfinite(event.speed)) {
            event.speed = 1.0;
        }
    }
    std::stable_sort(speedEvents.begin(), speedEvents.end(), [](const SpeedEvent& first, const SpeedEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_speedEvents = std::move(speedEvents);
    emit speedEventsChanged();
}

void EditorState::addTimingPointAtPlaybackPosition() {
    EventSnapshot before = eventSnapshot();
    TimingPoint point = timingPointAt(m_playbackPositionMilliseconds);
    point.timeMilliseconds = m_playbackPositionMilliseconds;
    const int insertionIndex = static_cast<int>(std::upper_bound(m_timingPoints.cbegin(), m_timingPoints.cend(), point.timeMilliseconds,
        [](const qint64 time, const TimingPoint& candidate) { return time < candidate.timeMilliseconds; }) - m_timingPoints.cbegin());
    m_timingPoints.insert(insertionIndex, point);
    setTimingPoints(m_timingPoints);
    setSelectedTimingPoint(insertionIndex);
    appendEventHistory(std::move(before));
}

void EditorState::addLaneEventAtPlaybackPosition() {
    EventSnapshot before = eventSnapshot();
    const LaneEvent event{.timeMilliseconds = m_playbackPositionMilliseconds, .lane = 0, .enabled = false};
    const int insertionIndex = static_cast<int>(std::upper_bound(m_laneEvents.cbegin(), m_laneEvents.cend(), event.timeMilliseconds,
        [](const qint64 time, const LaneEvent& candidate) { return time < candidate.timeMilliseconds; }) - m_laneEvents.cbegin());
    m_laneEvents.insert(insertionIndex, event);
    setLaneEvents(m_laneEvents);
    setSelectedLaneEvent(insertionIndex);
    appendEventHistory(std::move(before));
}

void EditorState::addSpeedEventAtPlaybackPosition() {
    EventSnapshot before = eventSnapshot();
    double speed = 1.0;
    for (const SpeedEvent& event : m_speedEvents) {
        if (event.timeMilliseconds > m_playbackPositionMilliseconds) {
            break;
        }
        speed = event.speed;
    }
    const SpeedEvent event{.timeMilliseconds = m_playbackPositionMilliseconds, .speed = speed};
    const int insertionIndex = static_cast<int>(std::upper_bound(m_speedEvents.cbegin(), m_speedEvents.cend(), event.timeMilliseconds,
        [](const qint64 time, const SpeedEvent& candidate) { return time < candidate.timeMilliseconds; }) - m_speedEvents.cbegin());
    m_speedEvents.insert(insertionIndex, event);
    setSpeedEvents(m_speedEvents);
    setSelectedSpeedEvent(insertionIndex);
    appendEventHistory(std::move(before));
}

void EditorState::removeTimingPoint(const int index) {
    if (index < 0 || index >= m_timingPoints.size()) {
        return;
    }
    if (m_timingPoints.size() == 1) {
        emit lastTimingPointRemovalRejected();
        return;
    }
    EventSnapshot before = eventSnapshot();
    m_timingPoints.removeAt(index);
    emit timingPointsChanged();
    emit divisorChanged(m_divisor);
    setSelectedTimingPoint(-1);
    appendEventHistory(std::move(before));
}

void EditorState::removeSpeedEvent(const int index) {
    if (index < 0 || index >= m_speedEvents.size()) {
        return;
    }
    EventSnapshot before = eventSnapshot();
    m_speedEvents.removeAt(index);
    emit speedEventsChanged();
    setSelectedSpeedEvent(-1);
    appendEventHistory(std::move(before));
}

void EditorState::removeSelection() {
    if (!m_selectedHitObjects.isEmpty()) {
        removeSelectedHitObjects();
        return;
    }
    if (selectedEventCount() == 0) {
        return;
    }
    if (m_selectedTimingPoints.size() >= m_timingPoints.size()) {
        emit lastTimingPointRemovalRejected();
        return;
    }

    EventSnapshot before = eventSnapshot();
    const auto removeIndexes = [](auto* events, QVector<int> indexes) {
        std::sort(indexes.begin(), indexes.end(), std::greater<>());
        for (const int index : indexes) {
            events->removeAt(index);
        }
    };
    const bool timingChanged = !m_selectedTimingPoints.isEmpty();
    const bool laneChanged = !m_selectedLaneEvents.isEmpty();
    const bool speedChanged = !m_selectedSpeedEvents.isEmpty();
    removeIndexes(&m_timingPoints, m_selectedTimingPoints);
    removeIndexes(&m_laneEvents, m_selectedLaneEvents);
    removeIndexes(&m_speedEvents, m_selectedSpeedEvents);
    if (timingChanged) {
        emit timingPointsChanged();
        emit divisorChanged(m_divisor);
    }
    if (laneChanged) {
        emit laneEventsChanged();
    }
    if (speedChanged) {
        emit speedEventsChanged();
    }
    setSelectedEvents({}, {}, {});
    appendEventHistory(std::move(before));
}

void EditorState::removeLaneEvent(const int index) {
    if (index < 0 || index >= m_laneEvents.size()) {
        return;
    }
    EventSnapshot before = eventSnapshot();
    m_laneEvents.removeAt(index);
    emit laneEventsChanged();
    setSelectedLaneEvent(-1);
    appendEventHistory(std::move(before));
}

void EditorState::setPlaybackPosition(const qint64 positionMilliseconds) {
    const qint64 clampedPosition = std::clamp(positionMilliseconds, qint64(0), m_chart.durationMilliseconds);
    if (m_playbackPositionMilliseconds == clampedPosition) {
        return;
    }
    m_playbackPositionMilliseconds = clampedPosition;
    emit playbackPositionChanged(m_playbackPositionMilliseconds);
}

void EditorState::setDivisor(const int divisor) {
    if (m_divisor == divisor || std::find(kDivisors.begin(), kDivisors.end(), divisor) == kDivisors.end()) {
        return;
    }
    m_divisor = divisor;
    emit divisorChanged(m_divisor);
}

void EditorState::increaseDivisor() {
    const auto iterator = std::find(kDivisors.begin(), kDivisors.end(), m_divisor);
    if (iterator != kDivisors.end() && std::next(iterator) != kDivisors.end()) {
        setDivisor(*std::next(iterator));
    }
}

void EditorState::decreaseDivisor() {
    const auto iterator = std::find(kDivisors.begin(), kDivisors.end(), m_divisor);
    if (iterator != kDivisors.begin() && iterator != kDivisors.end()) {
        setDivisor(*std::prev(iterator));
    }
}

void EditorState::seekByDivisor(const int direction) {
    if (direction == 0 || m_timingPoints.isEmpty()) {
        return;
    }
    constexpr double kGridTolerance = 0.0001;
    constexpr auto kRepeatWindow = std::chrono::milliseconds(50);
    const TimingPoint& point = timingPointAt(m_playbackPositionMilliseconds);
    const double beatDuration = 60000.0 / std::max(point.beatsPerMinute, 1.0);
    const double measureDuration = beatDuration * std::max(point.timeSignatureNumerator, 1) * 4.0
        / std::max(point.timeSignatureDenominator, 1);
    const double tickDuration = measureDuration / m_divisor;
    const double gridPosition = static_cast<double>(m_playbackPositionMilliseconds - point.timeMilliseconds) / tickDuration;
    const double nearestTick = std::round(gridPosition);
    const qint64 nearestTime = point.timeMilliseconds
        + static_cast<qint64>(std::llround(nearestTick * tickDuration));
    const bool isOnGrid = m_playbackPositionMilliseconds == nearestTime;
    const double targetTick = isOnGrid
        ? nearestTick + direction
        : direction > 0
            ? std::floor(gridPosition + kGridTolerance) + 1.0
            : std::ceil(gridPosition - kGridTolerance) - 1.0;
    qint64 target = point.timeMilliseconds + static_cast<qint64>(std::llround(targetTick * tickDuration));
    const auto now = std::chrono::steady_clock::now();
    const bool isStuck = std::abs(target - m_playbackPositionMilliseconds) < 1;
    const bool isRepeat = m_lastScrubDirection == direction
        && now - m_lastScrubTime < kRepeatWindow
        && target == m_lastScrubTargetMilliseconds;
    if (isStuck || isRepeat) {
        target = point.timeMilliseconds + static_cast<qint64>(std::llround(
            (targetTick + direction) * tickDuration));
    }
    target = std::clamp(target, qint64(0), m_chart.durationMilliseconds);
    m_lastScrubTime = now;
    m_lastScrubTargetMilliseconds = target;
    m_lastScrubDirection = direction;
    setPlaybackPosition(target);
}

void EditorState::setDifficulty(const Difficulty difficulty) {
    if (m_difficulty == difficulty) {
        return;
    }
    m_difficulty = difficulty;
    emit difficultyChanged(m_difficulty);
}

void EditorState::setTool(const EditorTool tool) {
    if (m_tool == tool) {
        return;
    }
    m_tool = tool;
    emit toolChanged(m_tool);
}

void EditorState::setFlatViewMode(const FlatViewMode mode) {
    if (m_flatViewMode == mode) {
        return;
    }
    m_flatViewMode = mode;
    emit flatViewModeChanged(m_flatViewMode);
}

void EditorState::addHitObject(ChartNote hitObject) {
    const QVector<int> selectionBefore = m_selectedHitObjects;
    hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds);
    hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, hitObject.endMilliseconds);
    constrainZoneDuration(hitObject);
    if (hitObject.id == 0) {
        hitObject.id = m_nextHitObjectId++;
    } else {
        m_nextHitObjectId = std::max(m_nextHitObjectId, hitObject.id + 1);
    }
    const int index = m_chart.notes.size();
    insertHitObject(index, hitObject);
    appendHistory({
        .operation = HistoryOperation::Add,
        .index = index,
        .after = hitObject,
        .selectionBefore = selectionBefore,
        .selectionAfter = {index},
    });
    m_pendingAddedHitObjectIndex = index;
    setSelectedHitObjects({index});
}

void EditorState::copyHitObjects(QVector<ChartNote> hitObjects) {
    if (hitObjects.isEmpty()) {
        return;
    }

    const QVector<int> selectionBefore = m_selectedHitObjects;
    HistoryEntry entry;
    entry.operation = HistoryOperation::AddBatch;
    entry.batchIndexes.reserve(hitObjects.size());
    entry.batchAfter.reserve(hitObjects.size());
    entry.selectionBefore = selectionBefore;
    QVector<int> copiedIndexes;
    copiedIndexes.reserve(hitObjects.size());
    for (ChartNote& hitObject : hitObjects) {
        const int index = m_chart.notes.size();
        hitObject.id = m_nextHitObjectId++;
        insertHitObject(index, hitObject);
        entry.batchIndexes.append(index);
        entry.batchAfter.append(hitObject);
        copiedIndexes.append(index);
    }
    entry.selectionAfter = copiedIndexes;
    appendHistory(std::move(entry));
    m_pendingAddedHitObjectIndex = -1;
    setSelectedHitObjects(copiedIndexes);
}

void EditorState::copySelectedHitObjects() {
    m_clipboardType = ClipboardType::None;
    m_clipboardHitObjects.clear();
    m_clipboardTimingPoints.clear();
    m_clipboardLaneEvents.clear();
    m_clipboardSpeedEvents.clear();
    if (!m_selectedHitObjects.isEmpty()) {
        m_clipboardHitObjects.reserve(m_selectedHitObjects.size());
        for (const int index : m_selectedHitObjects) {
            if (isValidHitObjectIndex(index)) {
                m_clipboardHitObjects.append(m_chart.notes.at(index));
            }
        }
        m_clipboardType = m_clipboardHitObjects.isEmpty() ? ClipboardType::None : ClipboardType::HitObjects;
    } else {
        for (const int index : m_selectedTimingPoints) {
            m_clipboardTimingPoints.append(m_timingPoints.at(index));
        }
        for (const int index : m_selectedLaneEvents) {
            m_clipboardLaneEvents.append(m_laneEvents.at(index));
        }
        for (const int index : m_selectedSpeedEvents) {
            m_clipboardSpeedEvents.append(m_speedEvents.at(index));
        }
        if (!m_clipboardTimingPoints.isEmpty() || !m_clipboardLaneEvents.isEmpty() || !m_clipboardSpeedEvents.isEmpty()) {
            m_clipboardType = ClipboardType::Events;
        }
    }
}

void EditorState::cutSelectedHitObjects() {
    if (m_selectedHitObjects.isEmpty() && selectedEventCount() == 0) {
        return;
    }

    copySelectedHitObjects();
    removeSelection();
}

void EditorState::pasteCopiedHitObjects() {
    if (m_clipboardType == ClipboardType::None) {
        return;
    }

    if (m_clipboardType == ClipboardType::Events) {
        EventSnapshot before = eventSnapshot();
        qint64 sourceStart = std::numeric_limits<qint64>::max();
        const auto findStart = [&sourceStart](const auto& events) {
            for (const auto& event : events) {
                sourceStart = std::min(sourceStart, event.timeMilliseconds);
            }
        };
        findStart(m_clipboardTimingPoints);
        findStart(m_clipboardLaneEvents);
        findStart(m_clipboardSpeedEvents);
        const qint64 offset = m_playbackPositionMilliseconds - sourceStart;
        QVector<int> timingIndexes;
        QVector<int> laneIndexes;
        QVector<int> speedIndexes;
        const auto insertEvents = [offset](auto* destination, const auto& source, QVector<int>* indexes) {
            using Event = typename std::decay_t<decltype(*destination)>::value_type;
            QVector<Event> ordered = source;
            std::stable_sort(ordered.begin(), ordered.end(), [](const Event& first, const Event& second) {
                return first.timeMilliseconds < second.timeMilliseconds;
            });
            for (Event event : ordered) {
                event.timeMilliseconds = std::max<qint64>(0, event.timeMilliseconds + offset);
                const int insertionIndex = static_cast<int>(std::upper_bound(destination->cbegin(), destination->cend(), event.timeMilliseconds,
                    [](const qint64 time, const Event& candidate) { return time < candidate.timeMilliseconds; }) - destination->cbegin());
                destination->insert(insertionIndex, event);
                indexes->append(insertionIndex);
            }
        };
        insertEvents(&m_timingPoints, m_clipboardTimingPoints, &timingIndexes);
        insertEvents(&m_laneEvents, m_clipboardLaneEvents, &laneIndexes);
        insertEvents(&m_speedEvents, m_clipboardSpeedEvents, &speedIndexes);
        if (!timingIndexes.isEmpty()) {
            emit timingPointsChanged();
            emit divisorChanged(m_divisor);
        }
        if (!laneIndexes.isEmpty()) {
            emit laneEventsChanged();
        }
        if (!speedIndexes.isEmpty()) {
            emit speedEventsChanged();
        }
        setSelectedEvents(std::move(timingIndexes), std::move(laneIndexes), std::move(speedIndexes));
        appendEventHistory(std::move(before));
        return;
    }
    if (m_clipboardHitObjects.isEmpty()) {
        return;
    }

    const auto firstHitObject = std::min_element(
        m_clipboardHitObjects.cbegin(),
        m_clipboardHitObjects.cend(),
        [](const ChartNote& first, const ChartNote& second) {
            return first.startMilliseconds < second.startMilliseconds;
        });
    const qint64 timeOffset = closestDivisorTime(m_playbackPositionMilliseconds)
        - firstHitObject->startMilliseconds;
    QVector<ChartNote> hitObjects = m_clipboardHitObjects;
    for (ChartNote& hitObject : hitObjects) {
        hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds + timeOffset);
        hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, hitObject.endMilliseconds + timeOffset);
        if (hitObject.kind == NoteKind::Tap || hitObject.kind == NoteKind::Flick) {
            hitObject.endMilliseconds = hitObject.startMilliseconds;
        }
    }
    copyHitObjects(std::move(hitObjects));
    emit hitObjectsPasted();
}

void EditorState::mirrorSelectedHitObjects() {
    if (m_selectedHitObjects.isEmpty()) {
        return;
    }

    QVector<ChartNote> hitObjects;
    hitObjects.reserve(m_selectedHitObjects.size());
    for (const int index : m_selectedHitObjects) {
        if (!isValidHitObjectIndex(index)) {
            return;
        }

        ChartNote hitObject = m_chart.notes.at(index);
        if (isSkyHitObject(hitObject)) {
            hitObject.startX = 1.0 - hitObject.startX;
            hitObject.endX = 1.0 - hitObject.endX;
            if (hitObject.kind == NoteKind::Sky) {
                mirrorZoneEasing(hitObject);
            } else {
                mirrorFlickDirection(hitObject);
            }
        } else {
            mirrorGroundHitObject(hitObject);
        }
        hitObjects.append(hitObject);
    }
    beginHitObjectBatchMove(m_selectedHitObjects);
    moveHitObjectBatch(m_selectedHitObjects, hitObjects);
    finishHitObjectBatchMove();
}

void EditorState::flipSelectedHitObjectsVertically() {
    if (m_selectedHitObjects.isEmpty()) {
        return;
    }

    qint64 selectionStartMilliseconds = std::numeric_limits<qint64>::max();
    qint64 selectionEndMilliseconds = std::numeric_limits<qint64>::lowest();
    for (const int index : m_selectedHitObjects) {
        if (!isValidHitObjectIndex(index)) {
            return;
        }

        const ChartNote& hitObject = m_chart.notes.at(index);
        selectionStartMilliseconds = std::min(selectionStartMilliseconds, hitObject.startMilliseconds);
        selectionEndMilliseconds = std::max(selectionEndMilliseconds, hitObject.endMilliseconds);
    }

    QVector<ChartNote> hitObjects;
    hitObjects.reserve(m_selectedHitObjects.size());
    for (const int index : m_selectedHitObjects) {
        ChartNote hitObject = m_chart.notes.at(index);
        flipHitObjectVertically(hitObject, selectionStartMilliseconds, selectionEndMilliseconds);
        hitObjects.append(hitObject);
    }
    beginHitObjectBatchMove(m_selectedHitObjects);
    moveHitObjectBatch(m_selectedHitObjects, hitObjects);
    finishHitObjectBatchMove();
}

void EditorState::toggleSelectedZoneGrouping() {
    QVector<int> indexes;
    QVector<ChartNote> zones;
    QSet<std::uint64_t> usedGroupIds;
    std::uint64_t availableGroupId = 0;

    for (const int index : m_selectedHitObjects) {
        if (isValidHitObjectIndex(index) && m_chart.notes.at(index).kind == NoteKind::Sky) {
            indexes.append(index);
            zones.append(m_chart.notes.at(index));
        }
    }
    if (zones.isEmpty()) {
        return;
    }
    for (const ChartNote& hitObject : m_chart.notes) {
        if (hitObject.kind == NoteKind::Sky) {
            usedGroupIds.insert(hitObject.groupId);
        }
    }
    const bool ungroup = zones.size() > 1
        && std::all_of(zones.cbegin(), zones.cend(), [&zones](const ChartNote& zone) {
            return zone.groupId == zones.front().groupId;
        });
    for (ChartNote& zone : zones) {
        while (usedGroupIds.contains(availableGroupId)) {
            ++availableGroupId;
        }
        zone.groupId = availableGroupId;
        if (ungroup) {
            usedGroupIds.insert(availableGroupId);
        }
    }
    editHitObjects(std::move(indexes), std::move(zones));
}

void EditorState::resnapAllHitObjects() {
    if (m_chart.notes.isEmpty() || m_timingPoints.isEmpty()) {
        return;
    }

    QVector<int> indexes;
    QVector<ChartNote> before;
    QVector<ChartNote> after;
    indexes.reserve(m_chart.notes.size());
    before.reserve(m_chart.notes.size());
    after.reserve(m_chart.notes.size());
    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& original = m_chart.notes.at(index);
        ChartNote snapped = original;
        snapped.startMilliseconds = nearestValidDivisorTime(m_timingPoints, original.startMilliseconds);
        if (snapped.kind == NoteKind::Tap || snapped.kind == NoteKind::Flick) {
            snapped.endMilliseconds = snapped.startMilliseconds;
        } else {
            snapped.endMilliseconds = nearestValidDivisorTime(m_timingPoints, original.endMilliseconds);
            if (snapped.endMilliseconds <= snapped.startMilliseconds) {
                snapped.endMilliseconds = nextValidDivisorTime(m_timingPoints, snapped.startMilliseconds);
            }
            constrainZoneDuration(snapped);
        }
        if (hitObjectsEqual(original, snapped)) {
            continue;
        }
        indexes.append(index);
        before.append(original);
        after.append(snapped);
    }
    if (indexes.isEmpty()) {
        return;
    }

    const QVector<int> selection = m_selectedHitObjects;
    replaceHitObjectsBatch(indexes, after);
    appendHistory({
        .operation = HistoryOperation::MoveBatch,
        .batchIndexes = std::move(indexes),
        .batchBefore = std::move(before),
        .batchAfter = std::move(after),
        .selectionBefore = selection,
        .selectionAfter = selection,
    });
}

void EditorState::removeHitObject(const int index) {
    if (!isValidHitObjectIndex(index)) {
        return;
    }

    const QVector<int> selectionBefore = m_selectedHitObjects;
    const ChartNote hitObject = m_chart.notes.at(index);
    eraseHitObject(index);
    appendHistory({
        .operation = HistoryOperation::Remove,
        .index = index,
        .before = hitObject,
        .selectionBefore = selectionBefore,
        .selectionAfter = {},
    });
    setSelectedHitObjects({});
}

void EditorState::removeSelectedHitObjects() {
    QVector<int> indexes = m_selectedHitObjects;
    if (indexes.isEmpty()) {
        return;
    }

    HistoryEntry entry;
    entry.operation = HistoryOperation::RemoveBatch;
    entry.selectionBefore = indexes;
    entry.batchIndexes = indexes;
    entry.batchBefore.reserve(indexes.size());
    for (const int index : indexes) {
        entry.batchBefore.append(m_chart.notes.at(index));
    }
    std::sort(indexes.begin(), indexes.end(), std::greater<>());
    for (const int index : indexes) {
        eraseHitObject(index);
    }
    appendHistory(std::move(entry));
    setSelectedHitObjects({});
}

void EditorState::cancelAddedHitObject(const int index) {
    if (!isValidHitObjectIndex(index)
        || m_undoHistory.isEmpty()
        || m_undoHistory.constLast().operation != HistoryOperation::Add
        || m_undoHistory.constLast().index != index) {
        return;
    }

    eraseHitObject(index);
    m_undoHistory.removeLast();
    m_pendingAddedHitObjectIndex = -1;
    if (m_activeMoveIndex == index) {
        m_activeMoveIndex = -1;
        m_activeMoveMergesAdd = false;
    }
    setSelectedHitObjects({});
    emit historyChanged(!m_undoHistory.isEmpty(), !m_redoHistory.isEmpty());
}

void EditorState::editHitObject(const int index, ChartNote hitObject) {
    if (!isValidHitObjectIndex(index)) {
        return;
    }

    const ChartNote original = m_chart.notes.at(index);
    hitObject.id = original.id;
    normalizeHitObject(hitObject);

    if (hitObjectsEqual(original, hitObject)) {
        return;
    }

    replaceHitObject(index, hitObject);
    appendHistory({
        .operation = HistoryOperation::Move,
        .index = index,
        .before = original,
        .after = hitObject,
        .selectionBefore = m_selectedHitObjects,
        .selectionAfter = m_selectedHitObjects,
    });
}

void EditorState::editHitObjects(QVector<int> indexes, QVector<ChartNote> hitObjects) {
    if (indexes.isEmpty() || indexes.size() != hitObjects.size()) {
        return;
    }
    HistoryEntry entry;
    entry.operation = HistoryOperation::MoveBatch;
    entry.selectionBefore = m_selectedHitObjects;
    entry.selectionAfter = m_selectedHitObjects;
    for (int itemIndex = 0; itemIndex < indexes.size(); ++itemIndex) {
        const int index = indexes.at(itemIndex);
        if (!isValidHitObjectIndex(index) || entry.batchIndexes.contains(index)) {
            continue;
        }
        const ChartNote original = m_chart.notes.at(index);
        ChartNote hitObject = hitObjects.at(itemIndex);
        hitObject.id = original.id;
        normalizeHitObject(hitObject);
        if (hitObjectsEqual(original, hitObject)) {
            continue;
        }
        entry.batchIndexes.append(index);
        entry.batchBefore.append(original);
        entry.batchAfter.append(hitObject);
    }
    if (entry.batchIndexes.isEmpty()) {
        return;
    }
    replaceHitObjectsBatch(entry.batchIndexes, entry.batchAfter);
    appendHistory(std::move(entry));
}

void EditorState::editTimingPoint(const int index, TimingPoint timingPoint) {
    if (index < 0 || index >= m_timingPoints.size()) {
        return;
    }

    normalizeEvent(timingPoint);
    if (m_timingPoints.at(index).timeMilliseconds == timingPoint.timeMilliseconds
        && m_timingPoints.at(index).beatsPerMinute == timingPoint.beatsPerMinute
        && m_timingPoints.at(index).timeSignatureNumerator == timingPoint.timeSignatureNumerator
        && m_timingPoints.at(index).timeSignatureDenominator == timingPoint.timeSignatureDenominator) {
        return;
    }

    EventSnapshot before = eventSnapshot();
    m_timingPoints[index] = timingPoint;
    std::sort(m_timingPoints.begin(), m_timingPoints.end(), [](const TimingPoint& first, const TimingPoint& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    const auto iterator = std::find_if(m_timingPoints.cbegin(), m_timingPoints.cend(), [&timingPoint](const TimingPoint& candidate) {
        return candidate.timeMilliseconds == timingPoint.timeMilliseconds
            && candidate.beatsPerMinute == timingPoint.beatsPerMinute
            && candidate.timeSignatureNumerator == timingPoint.timeSignatureNumerator
            && candidate.timeSignatureDenominator == timingPoint.timeSignatureDenominator;
    });
    const int updatedIndex = static_cast<int>(std::distance(m_timingPoints.cbegin(), iterator));
    emit timingPointsChanged();
    emit divisorChanged(m_divisor);
    setSelectedTimingPoint(updatedIndex);
    appendEventHistory(std::move(before));
}

void EditorState::editLaneEvent(const int index, LaneEvent laneEvent) {
    if (index < 0 || index >= m_laneEvents.size()) {
        return;
    }
    normalizeEvent(laneEvent);
    if (m_laneEvents.at(index).timeMilliseconds == laneEvent.timeMilliseconds
        && m_laneEvents.at(index).lane == laneEvent.lane
        && m_laneEvents.at(index).enabled == laneEvent.enabled) {
        return;
    }
    EventSnapshot before = eventSnapshot();
    m_laneEvents[index] = laneEvent;
    std::stable_sort(m_laneEvents.begin(), m_laneEvents.end(), [](const LaneEvent& first, const LaneEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    const auto iterator = std::find_if(m_laneEvents.cbegin(), m_laneEvents.cend(), [&laneEvent](const LaneEvent& candidate) {
        return candidate.timeMilliseconds == laneEvent.timeMilliseconds
            && candidate.lane == laneEvent.lane
            && candidate.enabled == laneEvent.enabled;
    });
    emit laneEventsChanged();
    setSelectedLaneEvent(static_cast<int>(std::distance(m_laneEvents.cbegin(), iterator)));
    appendEventHistory(std::move(before));
}

void EditorState::editSpeedEvent(const int index, SpeedEvent speedEvent) {
    if (index < 0 || index >= m_speedEvents.size() || !std::isfinite(speedEvent.speed)) {
        return;
    }
    normalizeEvent(speedEvent);
    if (m_speedEvents.at(index).timeMilliseconds == speedEvent.timeMilliseconds
        && m_speedEvents.at(index).speed == speedEvent.speed) {
        return;
    }
    EventSnapshot before = eventSnapshot();
    m_speedEvents[index] = speedEvent;
    std::stable_sort(m_speedEvents.begin(), m_speedEvents.end(), [](const SpeedEvent& first, const SpeedEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    const auto iterator = std::find_if(m_speedEvents.cbegin(), m_speedEvents.cend(), [&speedEvent](const SpeedEvent& candidate) {
        return candidate.timeMilliseconds == speedEvent.timeMilliseconds && candidate.speed == speedEvent.speed;
    });
    emit speedEventsChanged();
    setSelectedSpeedEvent(static_cast<int>(std::distance(m_speedEvents.cbegin(), iterator)));
    appendEventHistory(std::move(before));
}

void EditorState::editSelectedEvents(QVector<TimingPoint> timingPoints, QVector<LaneEvent> laneEvents,
    QVector<SpeedEvent> speedEvents) {
    if (timingPoints.size() != m_selectedTimingPoints.size() || laneEvents.size() != m_selectedLaneEvents.size()
        || speedEvents.size() != m_selectedSpeedEvents.size()
        || std::any_of(timingPoints.cbegin(), timingPoints.cend(), [](const TimingPoint& point) {
            return !std::isfinite(point.beatsPerMinute);
        }) || std::any_of(speedEvents.cbegin(), speedEvents.cend(), [](const SpeedEvent& event) {
            return !std::isfinite(event.speed);
        })) {
        return;
    }
    EventSnapshot before = eventSnapshot();
    const bool timingChanged = replaceSelectedEvents(m_timingPoints, m_selectedTimingPoints, std::move(timingPoints));
    const bool lanesChanged = replaceSelectedEvents(m_laneEvents, m_selectedLaneEvents, std::move(laneEvents));
    const bool speedsChanged = replaceSelectedEvents(m_speedEvents, m_selectedSpeedEvents, std::move(speedEvents));
    if (!timingChanged && !lanesChanged && !speedsChanged) {
        return;
    }
    m_selectedTimingPoint = m_selectedTimingPoints.isEmpty() ? -1 : m_selectedTimingPoints.constLast();
    m_selectedLaneEvent = m_selectedLaneEvents.isEmpty() ? -1 : m_selectedLaneEvents.constLast();
    m_selectedSpeedEvent = m_selectedSpeedEvents.isEmpty() ? -1 : m_selectedSpeedEvents.constLast();
    if (timingChanged) {
        emit timingPointsChanged();
        emit divisorChanged(m_divisor);
    }
    if (lanesChanged) {
        emit laneEventsChanged();
    }
    if (speedsChanged) {
        emit speedEventsChanged();
    }
    emit selectionChanged();
    appendEventHistory(std::move(before));
}

void EditorState::beginHitObjectMove(const int index) {
    if (!isValidHitObjectIndex(index)) {
        return;
    }

    m_moveSelectionStart = m_selectedHitObjects;
    m_activeMoveIndex = index;
    m_activeMoveMergesAdd = m_pendingAddedHitObjectIndex == index;
    m_pendingAddedHitObjectIndex = -1;
    m_moveStart = m_chart.notes.at(index);
    setSelectedHitObjects({index});
}

void EditorState::moveHitObject(const int index, ChartNote hitObject) {
    if (!isValidHitObjectIndex(index)) {
        return;
    }
    if (m_activeMoveIndex != index) {
        beginHitObjectMove(index);
    }
    if (hitObject.id == 0) {
        hitObject.id = m_chart.notes.at(index).id;
    }
    hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds);
    hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, hitObject.endMilliseconds);
    constrainZoneDuration(hitObject);
    replaceHitObject(index, hitObject);
}

void EditorState::finishHitObjectMove() {
    if (!isValidHitObjectIndex(m_activeMoveIndex)) {
        m_activeMoveIndex = -1;
        m_activeMoveMergesAdd = false;
        m_moveSelectionStart.clear();
        return;
    }

    const ChartNote hitObject = m_chart.notes.at(m_activeMoveIndex);
    if (!hitObjectsEqual(m_moveStart, hitObject)) {
        if (m_activeMoveMergesAdd
            && !m_undoHistory.isEmpty()
            && m_undoHistory.constLast().operation == HistoryOperation::Add
            && m_undoHistory.constLast().index == m_activeMoveIndex) {
            m_undoHistory.last().after = hitObject;
            emit historyChanged(true, !m_redoHistory.isEmpty());
        } else {
            appendHistory({
                .operation = HistoryOperation::Move,
                .index = m_activeMoveIndex,
                .before = m_moveStart,
                .after = hitObject,
                .selectionBefore = m_moveSelectionStart,
                .selectionAfter = m_selectedHitObjects,
            });
        }
    }
    m_activeMoveIndex = -1;
    m_activeMoveMergesAdd = false;
    m_moveSelectionStart.clear();
}

void EditorState::beginHitObjectBatchMove(QVector<int> indexes) {
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(), [this](const int index) {
        return !isValidHitObjectIndex(index);
    }), indexes.end());
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    if (indexes.isEmpty()) {
        return;
    }

    m_batchMoveSelectionStart = m_selectedHitObjects;
    m_activeBatchMoveIndexes = indexes;
    m_batchMoveStarts.clear();
    m_batchMoveStarts.reserve(indexes.size());
    for (const int index : indexes) {
        m_batchMoveStarts.append({
            .operation = HistoryOperation::Move,
            .index = index,
            .before = m_chart.notes.at(index),
        });
    }
    setSelectedHitObjects(indexes);
}

void EditorState::moveHitObjectBatch(const QVector<int> indexes, const QVector<ChartNote> hitObjects) {
    if (indexes.isEmpty() || indexes.size() != hitObjects.size()) {
        return;
    }
    if (m_activeBatchMoveIndexes != indexes) {
        beginHitObjectBatchMove(indexes);
    }
    if (m_activeBatchMoveIndexes != indexes) {
        return;
    }

    QVector<int> changedIndexes;
    QVector<ChartNote> changedHitObjects;
    changedIndexes.reserve(indexes.size());
    changedHitObjects.reserve(indexes.size());
    for (int itemIndex = 0; itemIndex < indexes.size(); ++itemIndex) {
        const int chartIndex = indexes.at(itemIndex);
        if (!isValidHitObjectIndex(chartIndex)) {
            continue;
        }
        ChartNote hitObject = hitObjects.at(itemIndex);
        hitObject.id = m_chart.notes.at(chartIndex).id;
        hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds);
        hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, hitObject.endMilliseconds);
        constrainZoneDuration(hitObject);
        if (hitObjectsEqual(m_chart.notes.at(chartIndex), hitObject)) {
            continue;
        }

        m_chart.notes[chartIndex] = hitObject;
        changedIndexes.append(chartIndex);
        changedHitObjects.append(hitObject);
    }
    if (!changedIndexes.isEmpty()) {
        emit hitObjectsBatchChanged(std::move(changedIndexes), std::move(changedHitObjects));
    }
}

void EditorState::finishHitObjectBatchMove() {
    if (m_activeBatchMoveIndexes.isEmpty() || m_batchMoveStarts.size() != m_activeBatchMoveIndexes.size()) {
        m_activeBatchMoveIndexes.clear();
        m_batchMoveStarts.clear();
        m_batchMoveSelectionStart.clear();
        return;
    }

    HistoryEntry entry;
    entry.operation = HistoryOperation::MoveBatch;
    entry.batchIndexes = m_activeBatchMoveIndexes;
    entry.selectionBefore = m_batchMoveSelectionStart;
    entry.selectionAfter = m_selectedHitObjects;
    entry.batchBefore.reserve(m_batchMoveStarts.size());
    entry.batchAfter.reserve(m_batchMoveStarts.size());
    bool changed = false;
    for (const HistoryEntry& start : m_batchMoveStarts) {
        const ChartNote after = m_chart.notes.at(start.index);
        entry.batchBefore.append(start.before);
        entry.batchAfter.append(after);
        changed = changed || !hitObjectsEqual(start.before, after);
    }
    if (changed) {
        appendHistory(std::move(entry));
        // Batch movement deliberately reports its visual updates separately
        // from this expensive whole-chart notification. Verification can wait
        // until the final drop position instead of rerunning for every note on
        // every mouse move.
        emit hitObjectsChanged();
    }

    m_activeBatchMoveIndexes.clear();
    m_batchMoveStarts.clear();
    m_batchMoveSelectionStart.clear();
}

void EditorState::undo() {
    if (m_undoHistory.isEmpty()) {
        return;
    }

    const HistoryEntry entry = m_undoHistory.takeLast();
    if (entry.operation == HistoryOperation::Events) {
        restoreEventSnapshot(entry.eventsBefore);
    } else {
        switch (entry.operation) {
    case HistoryOperation::Add:
        eraseHitObject(entry.index);
        break;
    case HistoryOperation::AddBatch:
        for (auto iterator = entry.batchIndexes.crbegin(); iterator != entry.batchIndexes.crend(); ++iterator) {
            eraseHitObject(*iterator);
        }
        break;
    case HistoryOperation::Remove:
        insertHitObject(entry.index, entry.before);
        break;
    case HistoryOperation::RemoveBatch:
        for (int index = 0; index < entry.batchIndexes.size(); ++index) {
            insertHitObject(entry.batchIndexes.at(index), entry.batchBefore.at(index));
        }
        break;
    case HistoryOperation::Move:
        replaceHitObject(entry.index, entry.before);
        break;
    case HistoryOperation::MoveBatch:
        replaceHitObjectsBatch(entry.batchIndexes, entry.batchBefore);
        break;
    case HistoryOperation::Events:
        break;
        }
        setSelectedHitObjects(entry.selectionBefore);
    }
    m_redoHistory.append(entry);
    emit historyChanged(!m_undoHistory.isEmpty(), !m_redoHistory.isEmpty());
}

void EditorState::redo() {
    if (m_redoHistory.isEmpty()) {
        return;
    }

    const HistoryEntry entry = m_redoHistory.takeLast();
    if (entry.operation == HistoryOperation::Events) {
        restoreEventSnapshot(entry.eventsAfter);
    } else {
        switch (entry.operation) {
    case HistoryOperation::Add:
        insertHitObject(entry.index, entry.after);
        break;
    case HistoryOperation::AddBatch:
        for (int index = 0; index < entry.batchIndexes.size(); ++index) {
            insertHitObject(entry.batchIndexes.at(index), entry.batchAfter.at(index));
        }
        break;
    case HistoryOperation::Remove:
        eraseHitObject(entry.index);
        break;
    case HistoryOperation::RemoveBatch:
        for (auto iterator = entry.batchIndexes.crbegin(); iterator != entry.batchIndexes.crend(); ++iterator) {
            eraseHitObject(*iterator);
        }
        break;
    case HistoryOperation::Move:
        replaceHitObject(entry.index, entry.after);
        break;
    case HistoryOperation::MoveBatch:
        replaceHitObjectsBatch(entry.batchIndexes, entry.batchAfter);
        break;
    case HistoryOperation::Events:
        break;
        }
        setSelectedHitObjects(entry.selectionAfter);
    }
    m_undoHistory.append(entry);
    emit historyChanged(!m_undoHistory.isEmpty(), !m_redoHistory.isEmpty());
}

void EditorState::setSelectedHitObjects(QVector<int> indexes) {
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(), [this](const int index) {
        return !isValidHitObjectIndex(index);
    }), indexes.end());
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    if (m_selectedHitObjects == indexes && m_selectedTimingPoints.isEmpty() && m_selectedLaneEvents.isEmpty()
        && m_selectedSpeedEvents.isEmpty()) {
        return;
    }

    m_selectedHitObjects = std::move(indexes);
    m_selectedTimingPoint = -1;
    m_selectedTimingPoints.clear();
    m_selectedLaneEvent = -1;
    m_selectedSpeedEvent = -1;
    m_selectedLaneEvents.clear();
    m_selectedSpeedEvents.clear();
    emit selectionChanged();
}

void EditorState::setSelectedTimingPoint(const int index) {
    setSelectedTimingPoints(index >= 0 && index < m_timingPoints.size() ? QVector<int>{index} : QVector<int>{});
}

void EditorState::setSelectedTimingPoints(QVector<int> indexes) {
    setSelectedEvents(std::move(indexes), {}, {});
}

void EditorState::setSelectedLaneEvent(const int index) {
    setSelectedEvents({}, index >= 0 ? QVector<int>{index} : QVector<int>{}, {});
}

void EditorState::setSelectedSpeedEvent(const int index) {
    setSelectedEvents({}, {}, index >= 0 ? QVector<int>{index} : QVector<int>{});
}

void EditorState::setSelectedEvents(QVector<int> timingIndexes, QVector<int> laneIndexes, QVector<int> speedIndexes) {
    const auto sanitize = [](QVector<int>* indexes, const int count) {
        indexes->erase(std::remove_if(indexes->begin(), indexes->end(), [count](const int index) {
            return index < 0 || index >= count;
        }), indexes->end());
        std::sort(indexes->begin(), indexes->end());
        indexes->erase(std::unique(indexes->begin(), indexes->end()), indexes->end());
    };
    sanitize(&timingIndexes, m_timingPoints.size());
    sanitize(&laneIndexes, m_laneEvents.size());
    sanitize(&speedIndexes, m_speedEvents.size());
    if (m_selectedTimingPoints == timingIndexes && m_selectedLaneEvents == laneIndexes
        && m_selectedSpeedEvents == speedIndexes && m_selectedHitObjects.isEmpty()) {
        return;
    }
    m_selectedTimingPoints = std::move(timingIndexes);
    m_selectedLaneEvents = std::move(laneIndexes);
    m_selectedSpeedEvents = std::move(speedIndexes);
    m_selectedTimingPoint = m_selectedTimingPoints.isEmpty() ? -1 : m_selectedTimingPoints.constLast();
    m_selectedLaneEvent = m_selectedLaneEvents.isEmpty() ? -1 : m_selectedLaneEvents.constLast();
    m_selectedSpeedEvent = m_selectedSpeedEvents.isEmpty() ? -1 : m_selectedSpeedEvents.constLast();
    m_selectedHitObjects.clear();
    emit selectionChanged();
}

const TimingPoint& EditorState::timingPointAt(const qint64 positionMilliseconds) const {
    const TimingPoint* current = &m_timingPoints.front();
    for (const TimingPoint& point : m_timingPoints) {
        if (point.timeMilliseconds > positionMilliseconds) {
            break;
        }
        current = &point;
    }

    return *current;
}

qint64 EditorState::closestDivisorTime(const qint64 positionMilliseconds) const {
    const TimingPoint& point = timingPointAt(positionMilliseconds);
    const double beatDurationMilliseconds = 60000.0 / std::max(point.beatsPerMinute, 1.0);
    const double measureDurationMilliseconds = beatDurationMilliseconds * point.timeSignatureNumerator * 4.0
        / std::max(point.timeSignatureDenominator, 1);
    const double divisorDurationMilliseconds = measureDurationMilliseconds / m_divisor;
    const double divisorIndex = std::round(
        static_cast<double>(positionMilliseconds - point.timeMilliseconds) / divisorDurationMilliseconds);

    return std::clamp(
        point.timeMilliseconds + static_cast<qint64>(std::llround(divisorIndex * divisorDurationMilliseconds)),
        qint64(0),
        m_chart.durationMilliseconds);
}

bool EditorState::isValidHitObjectIndex(const int index) const {
    return index >= 0 && index < m_chart.notes.size();
}

bool EditorState::hitObjectsEqual(const ChartNote& first, const ChartNote& second) {
    return first.id == second.id
        && first.groupId == second.groupId
        && first.auxiliary == second.auxiliary
        && first.startMilliseconds == second.startMilliseconds
        && first.endMilliseconds == second.endMilliseconds
        && first.startX == second.startX
        && first.endX == second.endX
        && first.startWidth == second.startWidth
        && first.endWidth == second.endWidth
        && first.kind == second.kind
        && first.side == second.side;
}

EditorState::EventSnapshot EditorState::eventSnapshot() const {
    return {
        .timingPoints = m_timingPoints,
        .laneEvents = m_laneEvents,
        .speedEvents = m_speedEvents,
        .selectedHitObjects = m_selectedHitObjects,
        .selectedTimingPoints = m_selectedTimingPoints,
        .selectedLaneEvents = m_selectedLaneEvents,
        .selectedSpeedEvents = m_selectedSpeedEvents,
        .selectedTimingPoint = m_selectedTimingPoint,
        .selectedLaneEvent = m_selectedLaneEvent,
        .selectedSpeedEvent = m_selectedSpeedEvent,
    };
}

void EditorState::restoreEventSnapshot(const EventSnapshot& snapshot) {
    m_timingPoints = snapshot.timingPoints;
    m_laneEvents = snapshot.laneEvents;
    m_speedEvents = snapshot.speedEvents;
    m_selectedHitObjects = snapshot.selectedHitObjects;
    m_selectedTimingPoint = snapshot.selectedTimingPoint;
    m_selectedTimingPoints = snapshot.selectedTimingPoints;
    m_selectedLaneEvents = snapshot.selectedLaneEvents;
    m_selectedSpeedEvents = snapshot.selectedSpeedEvents;
    m_selectedLaneEvent = snapshot.selectedLaneEvent;
    m_selectedSpeedEvent = snapshot.selectedSpeedEvent;
    emit timingPointsChanged();
    emit laneEventsChanged();
    emit speedEventsChanged();
    emit divisorChanged(m_divisor);
    emit selectionChanged();
}

void EditorState::appendEventHistory(EventSnapshot before) {
    appendHistory({
        .operation = HistoryOperation::Events,
        .eventsBefore = std::move(before),
        .eventsAfter = eventSnapshot(),
    });
}

void EditorState::appendHistory(HistoryEntry entry) {
    m_undoHistory.append(std::move(entry));
    m_redoHistory.clear();
    emit historyChanged(!m_undoHistory.isEmpty(), false);
}

void EditorState::insertHitObject(const int index, const ChartNote& hitObject) {
    const int insertionIndex = std::clamp(index, 0, static_cast<int>(m_chart.notes.size()));
    m_chart.notes.insert(insertionIndex, hitObject);
    emit hitObjectAdded(insertionIndex, hitObject);
    emit hitObjectsChanged();
}

void EditorState::eraseHitObject(const int index) {
    if (!isValidHitObjectIndex(index)) {
        return;
    }

    m_chart.notes.removeAt(index);
    emit hitObjectRemoved(index);
    emit hitObjectsChanged();
}

void EditorState::replaceHitObject(const int index, const ChartNote& hitObject) {
    if (!isValidHitObjectIndex(index) || hitObjectsEqual(m_chart.notes.at(index), hitObject)) {
        return;
    }

    m_chart.notes[index] = hitObject;
    emit hitObjectChanged(index, hitObject);
    emit hitObjectsChanged();
}

void EditorState::replaceHitObjectsBatch(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects) {
    if (indexes.size() != hitObjects.size()) {
        return;
    }

    QVector<int> changedIndexes;
    QVector<ChartNote> changedHitObjects;
    changedIndexes.reserve(indexes.size());
    changedHitObjects.reserve(hitObjects.size());
    for (int itemIndex = 0; itemIndex < indexes.size(); ++itemIndex) {
        const int chartIndex = indexes.at(itemIndex);
        const ChartNote& hitObject = hitObjects.at(itemIndex);
        if (!isValidHitObjectIndex(chartIndex) || hitObjectsEqual(m_chart.notes.at(chartIndex), hitObject)) {
            continue;
        }

        m_chart.notes[chartIndex] = hitObject;
        changedIndexes.append(chartIndex);
        changedHitObjects.append(hitObject);
    }
    if (changedIndexes.isEmpty()) {
        return;
    }

    emit hitObjectsBatchChanged(std::move(changedIndexes), std::move(changedHitObjects));
    emit hitObjectsChanged();
}

} // namespace infalsus::gui
