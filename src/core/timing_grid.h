#pragma once

#include "core/chart_project.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <optional>
#include <utility>

namespace infalsus {

inline constexpr std::array<int, 12> kSupportedDivisors{1, 2, 3, 4, 6, 8, 12, 16, 24, 32, 48, 64};

[[nodiscard]] inline double divisorDurationMilliseconds(const TimingPoint& point, const int divisor) {
    const double bpm = std::isfinite(point.beatsPerMinute) ? std::max(point.beatsPerMinute, 1.0)
                                                        : TimingPoint{}.beatsPerMinute;
    return 240000.0 / bpm / std::max(divisor, 1);
}

[[nodiscard]] inline double measureDurationMilliseconds(const TimingPoint& point) {
    return divisorDurationMilliseconds(point, point.timeSignatureDenominator)
        * std::max(point.timeSignatureNumerator, 1);
}

class TimingGrid {
public:
    explicit TimingGrid(const TimingPoint& point, const std::optional<int> divisor)
        : m_origin(point.timeMilliseconds), m_measureDuration(measureDurationMilliseconds(point)),
          m_tickDuration(divisor ? divisorDurationMilliseconds(point, *divisor) : m_measureDuration) {
        if (divisor) {
            const qint64 numerator = static_cast<qint64>(std::max(point.timeSignatureNumerator, 1)) * std::max(*divisor, 1);
            const int denominator = std::max(point.timeSignatureDenominator, 1);
            m_ticksPerMeasure = (numerator + denominator - 1) / denominator;
            m_uniform = numerator % denominator == 0;
        }
    }

    [[nodiscard]] qint64 subdivisionAtTick(const qint64 tick) const {
        const qint64 subdivision = tick % m_ticksPerMeasure;
        return subdivision < 0 ? subdivision + m_ticksPerMeasure : subdivision;
    }

    [[nodiscard]] double timeAtTick(const qint64 tick) const { return m_origin + offsetAtTick(tick); }

    [[nodiscard]] qint64 roundedTimeAtTick(const qint64 tick) const {
        return m_origin + static_cast<qint64>(std::llround(offsetAtTick(tick)));
    }

    [[nodiscard]] qint64 tickAtOrBefore(const double time) const {
        const double offset = time - m_origin;
        qint64 tick = 0;
        if (m_uniform) {
            tick = static_cast<qint64>(std::floor(offset / m_tickDuration));
        } else {
            const auto measure = static_cast<qint64>(std::floor(offset / m_measureDuration));
            const auto subdivision = static_cast<qint64>(std::floor(
                (offset - measure * m_measureDuration) / m_tickDuration));
            tick = measure * m_ticksPerMeasure + std::clamp(subdivision, qint64(0), m_ticksPerMeasure - 1);
        }
        if (timeAtTick(tick + 1) <= time) {
            return tick + 1;
        }
        return timeAtTick(tick) > time ? tick - 1 : tick;
    }

    [[nodiscard]] qint64 firstTickAtOrAfter(const qint64 time) const {
        const qint64 tick = tickAtOrBefore(static_cast<double>(time) - 0.5);
        return roundedTimeAtTick(tick) < time ? tick + 1 : tick;
    }

private:
    [[nodiscard]] double offsetAtTick(const qint64 tick) const {
        if (m_uniform) {
            return tick * m_tickDuration;
        }
        const qint64 subdivision = subdivisionAtTick(tick);
        const qint64 measure = (tick - subdivision) / m_ticksPerMeasure;
        return measure * m_measureDuration + subdivision * m_tickDuration;
    }

    qint64 m_origin;
    double m_measureDuration;
    double m_tickDuration;
    qint64 m_ticksPerMeasure = 1;
    bool m_uniform = true;
};

namespace detail {

[[nodiscard]] inline auto nextTimingPoint(const QVector<TimingPoint>& timingPoints, const qint64 time) {
    return std::upper_bound(timingPoints.cbegin(), timingPoints.cend(), time,
        [](const qint64 timestamp, const TimingPoint& point) { return timestamp < point.timeMilliseconds; });
}

[[nodiscard]] inline std::pair<qint64, qint64> surroundingGridTimes(const TimingGrid& grid, const qint64 time,
    const qint64 sectionEnd) {
    const qint64 tick = grid.firstTickAtOrAfter(time);
    return {grid.roundedTimeAtTick(tick - 1),
        std::min(grid.roundedTimeAtTick(tick), sectionEnd)};
}

} // namespace detail

[[nodiscard]] inline qint64 closestDivisorTime(const QVector<TimingPoint>& timingPoints, const qint64 time,
    const int divisor) {
    if (timingPoints.isEmpty()) {
        return std::max<qint64>(0, time);
    }
    const auto nextPoint = detail::nextTimingPoint(timingPoints, time);
    const TimingPoint& point = nextPoint == timingPoints.cbegin() ? timingPoints.front() : *std::prev(nextPoint);
    const TimingGrid grid(point, divisor);
    const qint64 sectionEnd = nextPoint == timingPoints.cend() ? std::numeric_limits<qint64>::max()
                                                               : nextPoint->timeMilliseconds;
    const auto [before, after] = detail::surroundingGridTimes(grid, time, sectionEnd);
    return std::max<qint64>(0, time - before < after - time ? before : after);
}

[[nodiscard]] inline qint64 adjacentGridTime(const QVector<TimingPoint>& timingPoints, const qint64 time,
    const int direction, const std::optional<int> divisor) {
    if (direction == 0 || timingPoints.isEmpty()) {
        return time;
    }
    const qint64 searchTime = direction > 0 ? time : time - 1;
    const auto nextPoint = detail::nextTimingPoint(timingPoints, searchTime);
    const TimingPoint& point = nextPoint == timingPoints.cbegin() ? timingPoints.front() : *std::prev(nextPoint);
    const TimingGrid grid(point, divisor);
    const qint64 tick = grid.firstTickAtOrAfter(direction > 0 ? time + 1 : time);
    if (direction < 0) {
        return std::max<qint64>(0, grid.roundedTimeAtTick(tick - 1));
    }
    const qint64 target = grid.roundedTimeAtTick(tick);
    return nextPoint == timingPoints.cend() ? target : std::min(target, nextPoint->timeMilliseconds);
}

[[nodiscard]] inline std::optional<double> unroundedValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    const auto nextPoint = detail::nextTimingPoint(timingPoints, timestampMilliseconds);
    if (nextPoint == timingPoints.cbegin()) {
        return std::nullopt;
    }

    const TimingPoint& point = *std::prev(nextPoint);
    std::optional<double> nearestTime;
    double nearestDistance = std::numeric_limits<double>::max();
    for (const int divisor : kSupportedDivisors) {
        const TimingGrid grid(point, divisor);
        const qint64 previousTick = grid.tickAtOrBefore(timestampMilliseconds);
        for (const qint64 tick : {previousTick, previousTick + 1}) {
            if (grid.roundedTimeAtTick(tick) != timestampMilliseconds) {
                continue;
            }
            const double time = grid.timeAtTick(tick);
            const double distance = std::abs(time - timestampMilliseconds);
            if (distance < nearestDistance) {
                nearestTime = time;
                nearestDistance = distance;
            }
        }
    }
    return nearestTime;
}

// Finds the closest timestamp that lies on at least one supported divisor of
// the active timing map. The result is calculated from absolute ticks, so each
// timestamp has the same nearest-millisecond rounding as editor navigation.
[[nodiscard]] inline qint64 nearestValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    const auto nextPoint = detail::nextTimingPoint(timingPoints, timestampMilliseconds);
    if (nextPoint == timingPoints.cbegin()) {
        return timingPoints.isEmpty() ? std::max<qint64>(0, timestampMilliseconds) : nextPoint->timeMilliseconds;
    }

    const TimingPoint& point = *std::prev(nextPoint);
    const qint64 sectionEnd = nextPoint == timingPoints.cend() ? std::numeric_limits<qint64>::max()
                                                               : nextPoint->timeMilliseconds;
    qint64 nearestTimestamp = point.timeMilliseconds;
    qint64 nearestDistance = std::numeric_limits<qint64>::max();
    for (const int divisor : kSupportedDivisors) {
        const TimingGrid grid(point, divisor);
        const auto [before, after] = detail::surroundingGridTimes(grid, timestampMilliseconds, sectionEnd);
        for (const qint64 candidate : {before, after}) {
            const qint64 distance = std::abs(candidate - timestampMilliseconds);
            if (distance < nearestDistance || (distance == nearestDistance && candidate < nearestTimestamp)) {
                nearestTimestamp = candidate;
                nearestDistance = distance;
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
    if (timingPoints.isEmpty()) {
        return timestampMilliseconds;
    }
    if (timestampMilliseconds < timingPoints.front().timeMilliseconds) {
        return timingPoints.front().timeMilliseconds;
    }
    qint64 nextTimestamp = std::numeric_limits<qint64>::max();
    for (const int divisor : kSupportedDivisors) {
        nextTimestamp = std::min(nextTimestamp, adjacentGridTime(timingPoints, timestampMilliseconds, 1, divisor));
    }

    return nextTimestamp == std::numeric_limits<qint64>::max() ? timestampMilliseconds : nextTimestamp;
}

[[nodiscard]] inline bool isValidDivisorTime(const QVector<TimingPoint>& timingPoints,
    const qint64 timestampMilliseconds) {
    return unroundedValidDivisorTime(timingPoints, timestampMilliseconds).has_value();
}

} // namespace infalsus
