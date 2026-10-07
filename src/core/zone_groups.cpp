#include "core/zone_groups.h"

#include <QtCore/QHash>
#include <QtCore/QMap>
#include <QtCore/QSet>

#include <algorithm>
#include <cmath>
#include <limits>

namespace infalsus {

namespace {

constexpr qint64 kEndpointTimeToleranceMilliseconds = 1;
constexpr double kEndpointEdgeTolerance = 0.0001;
constexpr qint64 kMinimumZoneDurationMilliseconds = 1;

struct ZoneEndpoint {
    qint64 timeMilliseconds = 0;
    int index = -1;
    bool atStart = true;
    double left = 0.0;
    double right = 0.0;
};

[[nodiscard]] ZoneEndpoint endpointFor(const ChartNote& note, const int index, const bool atStart) {
    const double center = atStart ? note.startX : note.endX;
    const double width = atStart ? note.startWidth : note.endWidth;

    return {.timeMilliseconds = atStart ? note.startMilliseconds : note.endMilliseconds,
        .index = index, .atStart = atStart, .left = center - width * 0.5, .right = center + width * 0.5};
}

[[nodiscard]] bool isConnected(const ZoneGroup& group, const QHash<int, QVector<int>>& neighbors) {
    QSet<int> visited;
    QVector<int> pending{group.indexes.front()};

    while (!pending.isEmpty()) {
        const int index = pending.takeLast();
        if (visited.contains(index)) {
            continue;
        }
        visited.insert(index);
        for (const int neighbor : neighbors.value(index)) {
            if (!visited.contains(neighbor)) {
                pending.append(neighbor);
            }
        }
    }

    return visited.size() == group.indexes.size();
}

} // namespace

QVector<ZoneGroup> analyzeZoneGroups(const ChartData& chart) {
    QMap<std::uint64_t, ZoneGroup> groups;
    QVector<ZoneGroup> result;

    for (int index = 0; index < chart.notes.size(); ++index) {
        const ChartNote& note = chart.notes.at(index);
        if (note.kind == NoteKind::Sky) {
            ZoneGroup& group = groups[note.groupId];
            group.groupId = note.groupId;
            group.indexes.append(index);
        }
    }
    result.reserve(groups.size());
    for (ZoneGroup& group : groups) {
        QVector<ZoneEndpoint> endpoints;
        QHash<int, QVector<int>> neighbors;
        endpoints.reserve(group.indexes.size() * 2);
        for (const int index : group.indexes) {
            const ChartNote& note = chart.notes.at(index);
            endpoints.append(endpointFor(note, index, true));
            endpoints.append(endpointFor(note, index, false));
        }
        std::stable_sort(endpoints.begin(), endpoints.end(), [](const ZoneEndpoint& first, const ZoneEndpoint& second) {
            return first.timeMilliseconds < second.timeMilliseconds;
        });
        for (int firstIndex = 0; firstIndex < endpoints.size(); ++firstIndex) {
            const ZoneEndpoint& first = endpoints.at(firstIndex);
            for (int secondIndex = firstIndex + 1; secondIndex < endpoints.size(); ++secondIndex) {
                const ZoneEndpoint& second = endpoints.at(secondIndex);
                if (second.timeMilliseconds - first.timeMilliseconds > kEndpointTimeToleranceMilliseconds) {
                    break;
                }
                if (first.index == second.index || first.atStart == second.atStart
                    || !std::isfinite(first.left) || !std::isfinite(first.right)
                    || !std::isfinite(second.left) || !std::isfinite(second.right)
                    || !(std::min(first.right, second.right) > std::max(first.left, second.left))) {
                    continue;
                }
                neighbors[first.index].append(second.index);
                neighbors[second.index].append(first.index);
                group.joints.append({.segment = {.left = std::max(first.left, second.left),
                    .right = std::min(first.right, second.right)},
                    .firstIndex = first.index, .secondIndex = second.index,
                    .firstAtStart = first.atStart, .secondAtStart = second.atStart,
                    .fullyShared = std::abs(first.left - second.left) <= kEndpointEdgeTolerance
                        && std::abs(first.right - second.right) <= kEndpointEdgeTolerance});
            }
        }
        group.connected = isConnected(group, neighbors);
        result.append(std::move(group));
    }

    return result;
}

QVector<ZoneSegment> mergeZoneSegments(QVector<ZoneSegment> segments) {
    QVector<ZoneSegment> result;

    std::sort(segments.begin(), segments.end(), [](const ZoneSegment& first, const ZoneSegment& second) {
        return first.left < second.left;
    });
    for (const ZoneSegment& segment : segments) {
        if (!result.isEmpty() && segment.left <= result.back().right) {
            result.back().right = std::max(result.back().right, segment.right);
        } else {
            result.append(segment);
        }
    }

    return result;
}

QVector<ChartNote> moveZoneJointTime(QVector<ChartNote> zones, const QVector<bool>& controlsStart,
    const qint64 requestedTimeMilliseconds) {
    qint64 minimumTime = 0;
    qint64 maximumTime = std::numeric_limits<qint64>::max();

    if (zones.size() != controlsStart.size()) {
        return zones;
    }
    for (int index = 0; index < zones.size(); ++index) {
        const ChartNote& zone = zones.at(index);
        if (controlsStart.at(index)) {
            maximumTime = std::min(maximumTime, zone.endMilliseconds - kMinimumZoneDurationMilliseconds);
        } else {
            minimumTime = std::max(minimumTime, zone.startMilliseconds + kMinimumZoneDurationMilliseconds);
        }
    }
    if (minimumTime > maximumTime) {
        return zones;
    }
    const qint64 sharedTime = std::clamp(requestedTimeMilliseconds, minimumTime, maximumTime);
    for (int index = 0; index < zones.size(); ++index) {
        if (controlsStart.at(index)) {
            zones[index].startMilliseconds = sharedTime;
        } else {
            zones[index].endMilliseconds = sharedTime;
        }
    }

    return zones;
}

} // namespace infalsus
