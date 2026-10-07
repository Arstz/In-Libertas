#pragma once

#include "core/chart_types.h"

namespace infalsus {

struct ZoneSegment {
    double left = 0.0;
    double right = 0.0;
};

struct ZoneJoint {
    ZoneSegment segment;
    int firstIndex = -1;
    int secondIndex = -1;
    bool firstAtStart = false;
    bool secondAtStart = true;
    bool fullyShared = false;
};

struct ZoneGroup {
    QVector<int> indexes;
    QVector<ZoneJoint> joints;
    std::uint64_t groupId = 0;
    bool connected = true;
};

[[nodiscard]] QVector<ZoneGroup> analyzeZoneGroups(const ChartData& chart);
[[nodiscard]] QVector<ZoneSegment> mergeZoneSegments(QVector<ZoneSegment> segments);
[[nodiscard]] QVector<int> linkedZoneEndpointIndexes(const ChartData& chart, int zoneIndex, bool atStart,
    bool fullySharedOnly);
[[nodiscard]] QVector<ChartNote> moveZoneJointTime(QVector<ChartNote> zones, const QVector<bool>& controlsStart,
    qint64 requestedTimeMilliseconds);

} // namespace infalsus
