#pragma once

#include "core/chart_project.h"

#include <QtCore/QHash>
#include <array>
#include <algorithm>
#include <deque>

namespace infalsus {

enum class PlaytestGrade : std::uint8_t {
    Pending, Miss, NearEarly, NearLate, ExactEarly, ExactLate, ExactPlus
};

struct PlaytestTick {
    int id = 0;
    double time = 0.0;
    int type = 1;
    PlaytestGrade grade = PlaytestGrade::Pending;
    bool finalized = false;
    bool excluded = false;
};

struct PlaytestNoteState {
    ChartNote note;
    QVector<PlaytestTick> ticks;
    qsizetype nextTick = 0;
    double successfulUntil = 0.0;
    double startCoordinate = 0.0;
    int firstLane = 0;
    int lastLane = 0;
    bool consumed = false;
    bool active = false;
    bool recontact = false;
    bool missed = false;
};

struct PlaytestJudgement {
    std::uint64_t noteId = 0;
    int tickId = 0;
    NoteKind kind = NoteKind::Unknown;
    FloorSide side = FloorSide::Unknown;
    int tickType = 0;
    PlaytestGrade grade = PlaytestGrade::Pending;
    double position = 0.5;
    double time = 0.0;
};

struct PlaytestResult {
    std::array<int, 7> counts{};
    std::array<std::array<int, 7>, 4> countsByKind{};
    int total = 0;
    int processed = 0;
    int combo = 0;
    int maxCombo = 0;
    qint64 score = 0;
};

class PlaytestSession {
public:
    void load(const ChartData& chart, QVector<TimingPoint> timingPoints,
        QVector<SpeedEvent> speedEvents, double startTime);
    void advance(double chartTime, double monotonicTime);
    void press(int lane, double chartTime, double monotonicTime, double eventTime = -1.0);
    void release(int lane, double chartTime, double monotonicTime, double eventTime = -1.0);
    void move(double normalizedDelta, double chartTime, double monotonicTime, double eventTime = -1.0);
    void clearInput(double monotonicTime);

    [[nodiscard]] const QVector<PlaytestNoteState>& notes() const { return m_notes; }
    [[nodiscard]] const PlaytestResult& result() const { return m_result; }
    [[nodiscard]] const std::array<bool, 6>& heldLanes() const { return m_held; }
    [[nodiscard]] double cursor() const { return std::clamp(m_cursor, 0.0, 1.0); }
    [[nodiscard]] double sweepLeft() const;
    [[nodiscard]] double sweepRight() const;
    [[nodiscard]] bool complete() const;
    [[nodiscard]] double endTime() const { return m_endTime; }
    [[nodiscard]] double time() const { return m_time; }
    [[nodiscard]] QVector<PlaytestJudgement> takeJudgements();

private:
    struct Lane {
        bool fresh = false;
        bool consumed = false;
        double transition = -1000000.0;
    };
    struct Motion { double time; double distance; };
    struct ScrollPoint { qint64 time; double coordinate; double speed; };
    struct SkyGroup {
        double credit = 300.0;
        double lockout = 0.0;
        double lastUpdate = 0.0;
        bool initialized = false;
        bool acquired = false;
    };

    void evaluate(double time, double monotonicTime);
    void advanceTo(double chartTime, double monotonicTime, bool evaluateAtEnd);
    void evaluateFloor(PlaytestNoteState& state, double time, double monotonicTime);
    void evaluateFlick(PlaytestNoteState& state, double time);
    void evaluateSky(PlaytestNoteState& state, double time);
    void commit(PlaytestNoteState& state, PlaytestTick& tick, PlaytestGrade grade, double time);
    void improve(PlaytestNoteState& state, PlaytestTick& tick, PlaytestGrade grade, double delta);
    void updateSky(SkyGroup& group, double time, bool contact);
    [[nodiscard]] bool skyContact(const ChartNote& note, double time, bool swept) const;
    [[nodiscard]] double coordinate(double time) const;
    void expireMotion(double monotonicTime);

    QVector<PlaytestNoteState> m_notes;
    std::array<QVector<int>, 4> m_sides;
    std::array<qsizetype, 4> m_firstPending{};
    QVector<ScrollPoint> m_scrollPoints{{0, 0.0, 1.0}};
    QHash<std::uint64_t, SkyGroup> m_groups;
    QHash<std::uint64_t, qint64> m_skyGroupEnds;
    QHash<std::uint64_t, bool> m_activeSkyGroups;
    QHash<std::uint64_t, bool> m_contactSkyGroups;
    std::array<Lane, 6> m_lanes{};
    std::array<bool, 6> m_held{};
    std::deque<Motion> m_leftMotion;
    std::deque<Motion> m_rightMotion;
    QVector<PlaytestJudgement> m_judgements;
    PlaytestResult m_result;
    double m_cursor = 0.5;
    double m_time = 0.0;
    double m_monotonicTime = 0.0;
    double m_endTime = 0.0;
};

} // namespace infalsus
