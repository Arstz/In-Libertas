#pragma once

#include "core/chart_project.h"

#include <QtCore/QObject>

#include <chrono>

namespace infalsus::gui {

enum class EditorTool {
    Place,
    Select,
    Move,
};

enum class FlatViewMode {
    Ground,
    Sky,
    Both,
};

class EditorState final : public QObject {
    Q_OBJECT

public:
    explicit EditorState(QObject* parent = nullptr);

    [[nodiscard]] const ChartData& chart() const;
    [[nodiscard]] const ChartMetadata& metadata() const;
    [[nodiscard]] const QVector<TimingPoint>& timingPoints() const;
    [[nodiscard]] const QVector<LaneEvent>& laneEvents() const;
    [[nodiscard]] const QVector<SpeedEvent>& speedEvents() const;
    [[nodiscard]] qint64 playbackPosition() const;
    [[nodiscard]] int divisor() const;
    [[nodiscard]] qint64 divisorDurationMilliseconds() const;
    [[nodiscard]] Difficulty difficulty() const;
    [[nodiscard]] EditorTool tool() const;
    [[nodiscard]] FlatViewMode flatViewMode() const;
    [[nodiscard]] const QVector<int>& selectedHitObjects() const;
    [[nodiscard]] int selectedTimingPoint() const;
    [[nodiscard]] const QVector<int>& selectedTimingPoints() const;
    [[nodiscard]] int selectedLaneEvent() const;
    [[nodiscard]] const QVector<int>& selectedLaneEvents() const;
    [[nodiscard]] int selectedSpeedEvent() const;
    [[nodiscard]] const QVector<int>& selectedSpeedEvents() const;
    [[nodiscard]] int selectedEventCount() const;

    void setChart(ChartData chart);
    void setMetadata(ChartMetadata metadata);
    void setTimingPoints(QVector<TimingPoint> timingPoints);
    void setLaneEvents(QVector<LaneEvent> laneEvents);
    void setSpeedEvents(QVector<SpeedEvent> speedEvents);
    void addTimingPointAtPlaybackPosition();
    void addLaneEventAtPlaybackPosition();
    void addSpeedEventAtPlaybackPosition();
    void removeTimingPoint(int index);
    void removeLaneEvent(int index);
    void removeSpeedEvent(int index);
    void removeSelection();
    void setPlaybackPosition(qint64 positionMilliseconds);
    void setDivisor(int divisor);
    void increaseDivisor();
    void decreaseDivisor();
    void seekByDivisor(int direction);
    void setDifficulty(Difficulty difficulty);
    void setTool(EditorTool tool);
    void setFlatViewMode(FlatViewMode mode);
    void addHitObject(ChartNote hitObject);
    void copyHitObjects(QVector<ChartNote> hitObjects);
    void copySelectedHitObjects();
    void cutSelectedHitObjects();
    void pasteCopiedHitObjects();
    void mirrorSelectedHitObjects();
    void flipSelectedHitObjectsVertically();
    void resnapAllHitObjects();
    void removeHitObject(int index);
    void removeSelectedHitObjects();
    void cancelAddedHitObject(int index);
    void editHitObject(int index, ChartNote hitObject);
    void editTimingPoint(int index, TimingPoint timingPoint);
    void editLaneEvent(int index, LaneEvent laneEvent);
    void editSpeedEvent(int index, SpeedEvent speedEvent);
    void beginHitObjectMove(int index);
    void moveHitObject(int index, ChartNote hitObject);
    void finishHitObjectMove();
    void beginHitObjectBatchMove(QVector<int> indexes);
    void moveHitObjectBatch(QVector<int> indexes, QVector<ChartNote> hitObjects);
    void finishHitObjectBatchMove();
    void undo();
    void redo();
    void setSelectedHitObjects(QVector<int> indexes);
    void setSelectedTimingPoint(int index);
    void setSelectedTimingPoints(QVector<int> indexes);
    void setSelectedLaneEvent(int index);
    void setSelectedSpeedEvent(int index);
    void setSelectedEvents(QVector<int> timingIndexes, QVector<int> laneIndexes, QVector<int> speedIndexes);

signals:
    void chartChanged();
    void metadataChanged();
    void timingPointsChanged();
    void laneEventsChanged();
    void speedEventsChanged();
    void lastTimingPointRemovalRejected();
    void playbackPositionChanged(qint64 positionMilliseconds);
    void divisorChanged(int divisor);
    void difficultyChanged(infalsus::Difficulty difficulty);
    void toolChanged(infalsus::gui::EditorTool tool);
    void flatViewModeChanged(infalsus::gui::FlatViewMode mode);
    void hitObjectAdded(int index, const ChartNote& hitObject);
    void hitObjectRemoved(int index);
    void hitObjectChanged(int index, const ChartNote& hitObject);
    void hitObjectsBatchChanged(QVector<int> indexes, QVector<ChartNote> hitObjects);
    void hitObjectsChanged();
    void hitObjectsPasted();
    void historyChanged(bool canUndo, bool canRedo);
    void selectionChanged();

private:
    enum class HistoryOperation {
        Add,
        AddBatch,
        Remove,
        RemoveBatch,
        Move,
        MoveBatch,
        Events,
    };

    struct EventSnapshot {
        QVector<TimingPoint> timingPoints;
        QVector<LaneEvent> laneEvents;
        QVector<SpeedEvent> speedEvents;
        QVector<int> selectedHitObjects;
        QVector<int> selectedTimingPoints;
        QVector<int> selectedLaneEvents;
        QVector<int> selectedSpeedEvents;
        int selectedTimingPoint = -1;
        int selectedLaneEvent = -1;
        int selectedSpeedEvent = -1;
    };

    struct HistoryEntry {
        HistoryOperation operation = HistoryOperation::Add;
        int index = -1;
        ChartNote before;
        ChartNote after;
        QVector<int> batchIndexes;
        QVector<ChartNote> batchBefore;
        QVector<ChartNote> batchAfter;
        QVector<int> selectionBefore;
        QVector<int> selectionAfter;
        EventSnapshot eventsBefore;
        EventSnapshot eventsAfter;
    };

    [[nodiscard]] const TimingPoint& timingPointAt(qint64 positionMilliseconds) const;
    [[nodiscard]] qint64 closestDivisorTime(qint64 positionMilliseconds) const;
    [[nodiscard]] bool isValidHitObjectIndex(int index) const;
    [[nodiscard]] static bool hitObjectsEqual(const ChartNote& first, const ChartNote& second);
    [[nodiscard]] EventSnapshot eventSnapshot() const;
    void restoreEventSnapshot(const EventSnapshot& snapshot);
    void appendEventHistory(EventSnapshot before);
    void appendHistory(HistoryEntry entry);
    void insertHitObject(int index, const ChartNote& hitObject);
    void eraseHitObject(int index);
    void replaceHitObject(int index, const ChartNote& hitObject);
    void replaceHitObjectsBatch(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects);

    ChartData m_chart;
    ChartMetadata m_metadata;
    QVector<TimingPoint> m_timingPoints;
    QVector<LaneEvent> m_laneEvents;
    QVector<SpeedEvent> m_speedEvents;
    qint64 m_playbackPositionMilliseconds = 0;
    int m_divisor = 4;
    Difficulty m_difficulty = Difficulty::Minimal;
    EditorTool m_tool = EditorTool::Place;
    FlatViewMode m_flatViewMode = FlatViewMode::Both;
    QVector<int> m_selectedHitObjects;
    QVector<HistoryEntry> m_undoHistory;
    QVector<HistoryEntry> m_redoHistory;
    QVector<ChartNote> m_clipboardHitObjects;
    enum class ClipboardType { None, HitObjects, Events };
    ClipboardType m_clipboardType = ClipboardType::None;
    QVector<TimingPoint> m_clipboardTimingPoints;
    QVector<LaneEvent> m_clipboardLaneEvents;
    QVector<SpeedEvent> m_clipboardSpeedEvents;
    QVector<HistoryEntry> m_batchMoveStarts;
    QVector<int> m_batchMoveSelectionStart;
    ChartNote m_moveStart;
    QVector<int> m_moveSelectionStart;
    std::uint64_t m_nextHitObjectId = 1;
    int m_activeMoveIndex = -1;
    QVector<int> m_activeBatchMoveIndexes;
    int m_pendingAddedHitObjectIndex = -1;
    int m_selectedTimingPoint = -1;
    QVector<int> m_selectedTimingPoints;
    int m_selectedLaneEvent = -1;
    QVector<int> m_selectedLaneEvents;
    int m_selectedSpeedEvent = -1;
    QVector<int> m_selectedSpeedEvents;
    qint64 m_lastScrubTargetMilliseconds = -1;
    std::chrono::steady_clock::time_point m_lastScrubTime;
    int m_lastScrubDirection = 0;
    bool m_activeMoveMergesAdd = false;
};

} // namespace infalsus::gui
