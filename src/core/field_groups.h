#pragma once

#include "core/chart_types.h"

namespace infalsus {

struct FieldSegment {
    double left = 0.0;
    double right = 0.0;
};

struct FieldJoint {
    FieldSegment segment;
    int firstIndex = -1;
    int secondIndex = -1;
    bool firstAtStart = false;
    bool secondAtStart = true;
    bool fullyShared = false;
};

struct FieldGroup {
    QVector<int> indexes;
    QVector<FieldJoint> joints;
    std::uint64_t groupId = 0;
    qint64 startMilliseconds = 0;
    bool connected = true;
};

[[nodiscard]] QVector<FieldGroup> analyzeFieldGroups(const ChartData& chart);
[[nodiscard]] QVector<std::uint64_t> chronologicalFieldGroupIds(const ChartData& chart);
[[nodiscard]] QVector<std::uint64_t> ascendingFieldGroupIds(const ChartData& chart);
[[nodiscard]] QVector<FieldSegment> mergeFieldSegments(QVector<FieldSegment> segments);
[[nodiscard]] QVector<int> linkedFieldEndpointIndexes(const ChartData& chart, int fieldIndex, bool atStart,
    bool fullySharedOnly);
[[nodiscard]] QVector<ChartNote> moveFieldJointTime(QVector<ChartNote> fields, const QVector<bool>& controlsStart,
    qint64 requestedTimeMilliseconds);

} // namespace infalsus
