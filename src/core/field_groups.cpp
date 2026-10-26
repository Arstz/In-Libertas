#include "core/field_groups.h"

#include <QtCore/QHash>
#include <QtCore/QMap>

#include <algorithm>
#include <cmath>
#include <limits>

namespace infalsus {

namespace {

constexpr qint64 kEndpointTimeToleranceMilliseconds = 1;
constexpr double kEndpointEdgeTolerance = 0.0001;
constexpr qint64 kMinimumFieldDurationMilliseconds = 2;

struct FieldEndpoint {
    qint64 timeMilliseconds = 0;
    int index = -1;
    bool atStart = true;
    double left = 0.0;
    double right = 0.0;
};

[[nodiscard]] FieldEndpoint endpointFor(const ChartNote& note, const int index, const bool atStart) {
    const double center = atStart ? note.startX : note.endX;
    const double width = atStart ? note.startWidth : note.endWidth;

    return {.timeMilliseconds = atStart ? note.startMilliseconds : note.endMilliseconds,
        .index = index, .atStart = atStart, .left = center - width * 0.5, .right = center + width * 0.5};
}

[[nodiscard]] bool isConnected(const FieldGroup& group, const ChartData& chart) {
    QVector<int> indexes = group.indexes;
    std::sort(indexes.begin(), indexes.end(), [&chart](const int first, const int second) {
        return chart.notes.at(first).startMilliseconds < chart.notes.at(second).startMilliseconds;
    });
    qint64 coveredEnd = chart.notes.at(indexes.front()).endMilliseconds;
    for (const int index : indexes) {
        const ChartNote& note = chart.notes.at(index);
        if (note.startMilliseconds > coveredEnd) {
            return false;
        }
        coveredEnd = std::max(coveredEnd, note.endMilliseconds);
    }
    return true;
}

} // namespace

QVector<FieldGroup> analyzeFieldGroups(const ChartData& chart) {
    QMap<std::uint64_t, FieldGroup> groups;
    QVector<FieldGroup> result;

    for (int index = 0; index < chart.notes.size(); ++index) {
        const ChartNote& note = chart.notes.at(index);
        if (note.kind == NoteKind::Sky) {
            FieldGroup& group = groups[note.groupId];
            group.startMilliseconds = group.indexes.isEmpty() ? note.startMilliseconds
                : std::min(group.startMilliseconds, note.startMilliseconds);
            group.groupId = note.groupId;
            group.indexes.append(index);
        }
    }
    result.reserve(groups.size());
    for (FieldGroup& group : groups) {
        QVector<FieldEndpoint> endpoints;
        endpoints.reserve(group.indexes.size() * 2);
        for (const int index : group.indexes) {
            const ChartNote& note = chart.notes.at(index);
            endpoints.append(endpointFor(note, index, true));
            endpoints.append(endpointFor(note, index, false));
        }
        std::stable_sort(endpoints.begin(), endpoints.end(), [](const FieldEndpoint& first, const FieldEndpoint& second) {
            return first.timeMilliseconds < second.timeMilliseconds;
        });
        for (int firstIndex = 0; firstIndex < endpoints.size(); ++firstIndex) {
            const FieldEndpoint& first = endpoints.at(firstIndex);
            for (int secondIndex = firstIndex + 1; secondIndex < endpoints.size(); ++secondIndex) {
                const FieldEndpoint& second = endpoints.at(secondIndex);
                if (second.timeMilliseconds - first.timeMilliseconds > kEndpointTimeToleranceMilliseconds) {
                    break;
                }
                if (first.index == second.index || first.atStart == second.atStart
                    || !std::isfinite(first.left) || !std::isfinite(first.right)
                    || !std::isfinite(second.left) || !std::isfinite(second.right)
                    || !(std::min(first.right, second.right) > std::max(first.left, second.left))) {
                    continue;
                }
                group.joints.append({.segment = {.left = std::max(first.left, second.left),
                    .right = std::min(first.right, second.right)},
                    .firstIndex = first.index, .secondIndex = second.index,
                    .firstAtStart = first.atStart, .secondAtStart = second.atStart,
                    .fullyShared = std::abs(first.left - second.left) <= kEndpointEdgeTolerance
                        && std::abs(first.right - second.right) <= kEndpointEdgeTolerance});
            }
        }
        group.connected = isConnected(group, chart);
        result.append(std::move(group));
    }

    return result;
}

QVector<std::uint64_t> chronologicalFieldGroupIds(const ChartData& chart) {
    QMap<std::uint64_t, qint64> starts;
    for (const ChartNote& note : chart.notes) {
        if (note.kind != NoteKind::Sky) {
            continue;
        }
        auto position = starts.find(note.groupId);
        if (position == starts.end()) {
            starts.insert(note.groupId, note.startMilliseconds);
        } else {
            position.value() = std::min(position.value(), note.startMilliseconds);
        }
    }
    QVector<std::uint64_t> ids = starts.keys();
    std::stable_sort(ids.begin(), ids.end(), [&starts](const std::uint64_t first, const std::uint64_t second) {
        return starts.value(first) < starts.value(second);
    });
    return ids;
}

QVector<std::uint64_t> ascendingFieldGroupIds(const ChartData& chart) {
    const QVector<std::uint64_t> chronologicalIds = chronologicalFieldGroupIds(chart);
    QVector<std::uint64_t> sortedIds = chronologicalIds;
    std::sort(sortedIds.begin(), sortedIds.end());
    QHash<std::uint64_t, std::uint64_t> replacements;
    for (int index = 0; index < chronologicalIds.size(); ++index) {
        replacements.insert(chronologicalIds.at(index), sortedIds.at(index));
    }
    QVector<std::uint64_t> ids;
    ids.reserve(chart.notes.size());
    for (const ChartNote& note : chart.notes) {
        ids.append(note.kind == NoteKind::Sky ? replacements.value(note.groupId) : note.groupId);
    }
    return ids;
}

QVector<FieldSegment> mergeFieldSegments(QVector<FieldSegment> segments) {
    QVector<FieldSegment> result;

    std::sort(segments.begin(), segments.end(), [](const FieldSegment& first, const FieldSegment& second) {
        return first.left < second.left;
    });
    for (const FieldSegment& segment : segments) {
        if (!result.isEmpty() && segment.left <= result.back().right) {
            result.back().right = std::max(result.back().right, segment.right);
        } else {
            result.append(segment);
        }
    }

    return result;
}

QVector<int> linkedFieldEndpointIndexes(const ChartData& chart, const int fieldIndex, const bool atStart,
    const bool fullySharedOnly) {
    QVector<int> indexes;

    if (fieldIndex < 0 || fieldIndex >= chart.notes.size() || chart.notes.at(fieldIndex).kind != NoteKind::Sky) {
        return indexes;
    }
    const ChartNote& field = chart.notes.at(fieldIndex);
    const FieldEndpoint endpoint = endpointFor(field, fieldIndex, atStart);
    indexes.append(fieldIndex);
    for (int index = 0; index < chart.notes.size(); ++index) {
        const ChartNote& candidate = chart.notes.at(index);
        if (index == fieldIndex || candidate.kind != NoteKind::Sky || candidate.groupId != field.groupId) {
            continue;
        }
        const FieldEndpoint candidateEndpoint = endpointFor(candidate, index, !atStart);
        if (std::abs(candidateEndpoint.timeMilliseconds - endpoint.timeMilliseconds) > kEndpointTimeToleranceMilliseconds
            || !std::isfinite(endpoint.left) || !std::isfinite(endpoint.right)
            || !std::isfinite(candidateEndpoint.left) || !std::isfinite(candidateEndpoint.right)) {
            continue;
        }
        const bool shared = fullySharedOnly
            ? std::abs(candidateEndpoint.left - endpoint.left) <= kEndpointEdgeTolerance
                && std::abs(candidateEndpoint.right - endpoint.right) <= kEndpointEdgeTolerance
            : std::min(candidateEndpoint.right, endpoint.right) > std::max(candidateEndpoint.left, endpoint.left);
        if (shared) {
            indexes.append(index);
        }
    }
    std::sort(indexes.begin(), indexes.end());

    return indexes;
}

QVector<ChartNote> moveFieldJointTime(QVector<ChartNote> fields, const QVector<bool>& controlsStart,
    const qint64 requestedTimeMilliseconds) {
    qint64 minimumTime = 0;
    qint64 maximumTime = std::numeric_limits<qint64>::max();

    if (fields.size() != controlsStart.size()) {
        return fields;
    }
    for (int index = 0; index < fields.size(); ++index) {
        const ChartNote& field = fields.at(index);
        if (controlsStart.at(index)) {
            maximumTime = std::min(maximumTime, field.endMilliseconds - kMinimumFieldDurationMilliseconds);
        } else {
            minimumTime = std::max(minimumTime, field.startMilliseconds + kMinimumFieldDurationMilliseconds);
        }
    }
    if (minimumTime > maximumTime) {
        return fields;
    }
    const qint64 sharedTime = std::clamp(requestedTimeMilliseconds, minimumTime, maximumTime);
    for (int index = 0; index < fields.size(); ++index) {
        if (controlsStart.at(index)) {
            fields[index].startMilliseconds = sharedTime;
        } else {
            fields[index].endMilliseconds = sharedTime;
        }
    }

    return fields;
}

} // namespace infalsus
