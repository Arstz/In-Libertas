#pragma once

#include "core/chart_project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>

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

[[nodiscard]] inline std::optional<double> unroundedValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    const auto nextPoint = std::upper_bound(timingPoints.cbegin(), timingPoints.cend(), timestampMilliseconds,
        [](const qint64 timestamp, const TimingPoint& point) {
            return timestamp < point.timeMilliseconds;
        });
    if (nextPoint == timingPoints.cbegin()) {
        return std::nullopt;
    }

    const TimingPoint& point = *std::prev(nextPoint);
    const double measureDuration = detail::measureDurationMilliseconds(point);
    std::optional<double> nearestTime;
    double nearestDistance = std::numeric_limits<double>::max();
    for (const int divisor : kSupportedDivisors) {
        const double tickDuration = measureDuration / divisor;
        const qint64 tick = static_cast<qint64>(std::llround(
            (timestampMilliseconds - point.timeMilliseconds) / tickDuration));
        const double relativeTime = tick * tickDuration;
        if (point.timeMilliseconds + static_cast<qint64>(std::llround(relativeTime)) != timestampMilliseconds) {
            continue;
        }
        const double time = point.timeMilliseconds + relativeTime;
        const double distance = std::abs(time - timestampMilliseconds);
        if (distance < nearestDistance) {
            nearestTime = time;
            nearestDistance = distance;
        }
    }
    return nearestTime;
}

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

[[nodiscard]] inline qint64 translateDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 sourceTime, const qint64 sourceAnchor, const qint64 destinationAnchor) {
    const qint64 shiftedTime = std::max<qint64>(0, sourceTime + destinationAnchor - sourceAnchor);
    const auto sourceGridAnchor = unroundedValidDivisorTime(timingPoints, sourceAnchor);
    const auto destinationGridAnchor = unroundedValidDivisorTime(timingPoints, destinationAnchor);
    const auto sourceGridTime = unroundedValidDivisorTime(timingPoints, sourceTime);
    if (!sourceGridAnchor || !destinationGridAnchor || !sourceGridTime) {
        return shiftedTime;
    }
    constexpr double kGridToleranceMilliseconds = 0.000001;
    const double translatedGridTime = *destinationGridAnchor + (*sourceGridTime - *sourceGridAnchor);
    const qint64 roundedTime = nearestValidDivisorTime(timingPoints,
        static_cast<qint64>(std::llround(translatedGridTime)));
    const auto destinationGridTime = unroundedValidDivisorTime(timingPoints, roundedTime);
    return destinationGridTime
            && std::abs(*destinationGridTime - translatedGridTime) <= kGridToleranceMilliseconds
        ? roundedTime : shiftedTime;
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
    return unroundedValidDivisorTime(timingPoints, timestampMilliseconds).has_value();
}

} // namespace infalsus
