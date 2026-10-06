#pragma once

#include "core/chart_project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace infalsus {

inline constexpr std::array<int, 12> kSupportedDivisors{1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};

namespace detail {

[[nodiscard]] inline double measureDurationMilliseconds(const TimingPoint& point) {
    return (60000.0 / std::max(point.beatsPerMinute, 1.0))
        * std::max(point.timeSignatureNumerator, 1) * 4.0
        / std::max(point.timeSignatureDenominator, 1);
}

[[nodiscard]] inline bool isInsideTimingSection(const QVector<TimingPoint>& timingPoints, const int pointIndex,
    const qint64 timestampMilliseconds) {
    return timestampMilliseconds >= timingPoints.at(pointIndex).timeMilliseconds
        && (pointIndex + 1 >= timingPoints.size()
            || timestampMilliseconds < timingPoints.at(pointIndex + 1).timeMilliseconds);
}

} // namespace detail

// Finds the closest timestamp that lies on at least one supported divisor of
// the active timing map. The result is calculated from absolute ticks, so each
// timestamp has the same nearest-millisecond rounding as editor navigation.
[[nodiscard]] inline qint64 nearestValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    if (timingPoints.isEmpty()) {
        return std::max<qint64>(0, timestampMilliseconds);
    }

    qint64 nearestTimestamp = std::max<qint64>(0, timestampMilliseconds);
    qint64 nearestDistance = std::numeric_limits<qint64>::max();
    for (int pointIndex = 0; pointIndex < timingPoints.size(); ++pointIndex) {
        const TimingPoint& point = timingPoints.at(pointIndex);
        const double measureDuration = detail::measureDurationMilliseconds(point);
        for (const int divisor : kSupportedDivisors) {
            const double tickDuration = measureDuration / divisor;
            const double gridPosition = (timestampMilliseconds - point.timeMilliseconds) / tickDuration;
            const qint64 centerTick = std::max<qint64>(0, static_cast<qint64>(std::llround(gridPosition)));
            for (const qint64 tick : {std::max<qint64>(0, centerTick - 1), centerTick, centerTick + 1}) {
                const qint64 candidate = point.timeMilliseconds
                    + static_cast<qint64>(std::llround(tick * tickDuration));
                if (!detail::isInsideTimingSection(timingPoints, pointIndex, candidate)) {
                    continue;
                }
                const qint64 distance = std::abs(candidate - timestampMilliseconds);
                if (distance < nearestDistance || (distance == nearestDistance && candidate < nearestTimestamp)) {
                    nearestTimestamp = candidate;
                    nearestDistance = distance;
                }
            }
        }
    }

    return nearestTimestamp;
}

[[nodiscard]] inline qint64 nextValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    qint64 nextTimestamp = std::numeric_limits<qint64>::max();
    for (int pointIndex = 0; pointIndex < timingPoints.size(); ++pointIndex) {
        const TimingPoint& point = timingPoints.at(pointIndex);
        const double measureDuration = detail::measureDurationMilliseconds(point);
        for (const int divisor : kSupportedDivisors) {
            const double tickDuration = measureDuration / divisor;
            const double gridPosition = (timestampMilliseconds - point.timeMilliseconds) / tickDuration;
            const qint64 tick = std::max<qint64>(0, static_cast<qint64>(std::floor(gridPosition)) + 1);
            const qint64 candidate = point.timeMilliseconds + static_cast<qint64>(std::llround(tick * tickDuration));
            if (candidate > timestampMilliseconds && detail::isInsideTimingSection(timingPoints, pointIndex, candidate)) {
                nextTimestamp = std::min(nextTimestamp, candidate);
            }
        }
    }

    return nextTimestamp == std::numeric_limits<qint64>::max() ? timestampMilliseconds : nextTimestamp;
}

[[nodiscard]] inline bool isValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    return !timingPoints.isEmpty() && nearestValidDivisorTime(timingPoints, timestampMilliseconds) == timestampMilliseconds;
}

} // namespace infalsus
