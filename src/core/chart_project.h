#pragma once

#include "core/chart_types.h"

#include <QtCore/QString>
#include <QtCore/QVector>

#include <array>

namespace infalsus {

enum class Difficulty {
    Minimal,
    Evolved,
    Ultimate,
    Forbidden,
};

struct ChartMetadata {
    QString artistName;
    QString songName;
    QString chartDesigner;
    QString jacketDesigner;
    double previewStartSeconds = 0.0;
    double previewEndSeconds = 0.0;
    QString characterIdentifier;
    QString gameplayBackground = QStringLiteral("0");
};

struct DifficultyMetadata {
    int rating = 1;
};

struct TimingPoint {
    qint64 timeMilliseconds = 0;
    double beatsPerMinute = 120.0;
    int timeSignatureNumerator = 4;
    int timeSignatureDenominator = 4;
};

struct LaneEvent {
    qint64 timeMilliseconds = 0;
    int lane = 0;
    bool enabled = false;
};

// The ICP1 type-0 event payload is a double precision conveyor-speed
// multiplier.  It changes only how the 3D conveyor progresses; chart time and
// every other editor view remain in ordinary milliseconds.
struct SpeedEvent {
    qint64 timeMilliseconds = 0;
    double speed = 1.0;
};

struct DifficultyChart {
    Difficulty difficulty = Difficulty::Minimal;
    DifficultyMetadata metadata;
    ChartData hitObjects;
    QVector<TimingPoint> timingPoints;
    QVector<LaneEvent> laneEvents;
    QVector<SpeedEvent> speedEvents;
};

struct ChartProject {
    ChartMetadata metadata;
    QString jacketPath;
    QString songPath;
    QString chartId;
    std::array<DifficultyChart, 4> difficulties;
};

[[nodiscard]] QString difficultyName(Difficulty difficulty);
[[nodiscard]] Difficulty difficultyForIndex(int index);
[[nodiscard]] int difficultyIndex(Difficulty difficulty);

} // namespace infalsus
