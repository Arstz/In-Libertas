#include "core/playtest_session.h"
#include "core/chart_document.h"

#include <cmath>
#include <utility>

namespace infalsus {
namespace {

constexpr std::uint64_t kEarlyMask = 0x1FDF9F1E3C181ULL;
constexpr std::uint64_t kLateMask = 0xFCF8F060C080ULL;
constexpr double kSkyContactTolerance = 0.02;
constexpr double kCursorSampleInterval = 8.0;

bool allowed(std::uint64_t mask, PlaytestGrade grade, PlaytestGrade previous) {
    return (mask & (1ULL << (7 * static_cast<int>(grade) + static_cast<int>(previous)))) != 0;
}

PlaytestGrade timingGrade(double magnitude, double delta, double scale = 1.0) {
    if (magnitude <= 25.0 * scale) {
        return PlaytestGrade::ExactPlus;
    }
    if (magnitude <= 50.0 * scale) {
        return delta >= 0.0 ? PlaytestGrade::ExactEarly : PlaytestGrade::ExactLate;
    }
    if (magnitude <= 100.0 * scale) {
        return delta >= 0.0 ? PlaytestGrade::NearEarly : PlaytestGrade::NearLate;
    }
    return PlaytestGrade::Miss;
}

double ease(std::uint32_t auxiliary, bool right, double progress) {
    const auto mode = auxiliary & (right ? 0xE0U : 0x1CU);
    const double p = std::clamp(progress, 0.0, 1.0);
    constexpr double halfPi = 1.5707963267948966;
    if (mode == (right ? 0x40U : 0x08U)) {
        return std::sin(p * halfPi);
    }
    if (mode == (right ? 0x80U : 0x10U)) {
        return 1.0 - std::cos(p * halfPi);
    }
    return p;
}

std::pair<double, double> skyBounds(const ChartNote& note, const double time) {
    const double p = (time - note.startMilliseconds)
        / std::max<double>(1.0, note.endMilliseconds - note.startMilliseconds);
    const double left = note.startX - note.startWidth * 0.5
        + (note.endX - note.endWidth * 0.5 - note.startX + note.startWidth * 0.5) * ease(note.auxiliary, false, p);
    const double right = note.startX + note.startWidth * 0.5
        + (note.endX + note.endWidth * 0.5 - note.startX - note.startWidth * 0.5) * ease(note.auxiliary, true, p);
    return {left, right};
}

} // namespace

void PlaytestSession::load(const ChartData& chart, QVector<TimingPoint> timingPoints,
    QVector<SpeedEvent> speedEvents, const double startTime) {
    *this = PlaytestSession{};
    std::stable_sort(speedEvents.begin(), speedEvents.end(), [](const auto& a, const auto& b) {
        return a.timeMilliseconds < b.timeMilliseconds;
    });
    for (const auto& event : speedEvents) {
        if (event.timeMilliseconds < 0 || !std::isfinite(event.speed)) {
            continue;
        }
        ScrollPoint& previous = m_scrollPoints.last();
        const double value = previous.coordinate + (event.timeMilliseconds - previous.time) * previous.speed;
        if (event.timeMilliseconds == previous.time) {
            previous.speed = event.speed;
        } else {
            m_scrollPoints.append({event.timeMilliseconds, value, event.speed});
        }
    }
    std::stable_sort(timingPoints.begin(), timingPoints.end(), [](const auto& a, const auto& b) {
        return a.timeMilliseconds < b.timeMilliseconds;
    });
    const ChartData snapshot = ChartDocument::playbackSnapshot(chart);
    m_time = startTime;
    m_endTime = std::max<double>(chart.durationMilliseconds, startTime);
    int nextTickId = 0;
    for (const ChartNote& note : snapshot.notes) {
        if (note.kind == NoteKind::Unknown || note.side == FloorSide::Unknown) {
            continue;
        }
        PlaytestNoteState state;
        state.note = note;
        state.startCoordinate = coordinate(note.startMilliseconds);
        state.successfulUntil = note.startMilliseconds;
        if (note.kind == NoteKind::Sky) {
            m_skyGroupEnds[note.groupId] = std::max(m_skyGroupEnds.value(note.groupId), note.endMilliseconds);
        }
        if (note.side == FloorSide::LeftOuter) {
            state.firstLane = state.lastLane = 0;
        } else if (note.side == FloorSide::RightOuter) {
            state.firstLane = state.lastLane = 5;
        } else {
            state.firstLane = std::clamp(static_cast<int>(std::llround(note.startX * 4.0)), 1, 4);
            state.lastLane = std::clamp(state.firstLane
                + std::max(1, static_cast<int>(std::llround(note.startWidth * 4.0))) - 1, state.firstLane, 4);
        }
        double bpm = timingPoints.isEmpty() ? 120.0 : timingPoints.front().beatsPerMinute;
        for (const auto& point : timingPoints) {
            if (point.timeMilliseconds > note.startMilliseconds) {
                break;
            }
            bpm = point.beatsPerMinute;
        }
        if (!std::isfinite(bpm) || bpm <= 0.0) {
            bpm = 120.0;
        }
        const double step = 30000.0 / (bpm >= 249.99 ? bpm * 0.5 : bpm);
        const auto append = [&](double time, int type) {
            const bool excluded = time < startTime;
            state.ticks.append({nextTickId++, time, type, PlaytestGrade::Pending, excluded, excluded});
            if (!excluded) {
                ++m_result.total;
            }
        };
        if (note.kind == NoteKind::Tap || note.kind == NoteKind::Flick) {
            append(note.startMilliseconds, note.kind == NoteKind::Tap ? 1 : 5);
        } else {
            if (note.kind == NoteKind::Hold) {
                append(note.startMilliseconds, (note.auxiliary & 0x100U) != 0 ? 3 : 1);
            }
            double last = note.startMilliseconds;
            for (qint64 index = 1;; ++index) {
                const double time = note.startMilliseconds + index * step;
                if (time > note.endMilliseconds) {
                    break;
                }
                append(time, 3);
                last = time;
            }
            if (note.endMilliseconds - last >= 2.0) {
                append(note.endMilliseconds, 3);
            }
        }
        while (state.nextTick < state.ticks.size() && state.ticks[state.nextTick].finalized) {
            ++state.nextTick;
        }
        state.consumed = state.nextTick == state.ticks.size();
        m_sides[static_cast<int>(note.side)].append(static_cast<int>(m_notes.size()));
        m_endTime = std::max(m_endTime, static_cast<double>(note.endMilliseconds));
        m_notes.append(std::move(state));
    }
    QVector<double> flickTimes;
    for (const auto& state : m_notes) {
        if (state.note.kind == NoteKind::Flick) {
            flickTimes.append(state.note.startMilliseconds);
        }
    }
    for (auto& state : m_notes) {
        if (state.note.kind != NoteKind::Sky) {
            continue;
        }
        for (auto& tick : state.ticks) {
            if (std::abs(tick.time - state.note.startMilliseconds) >= 2.0
                && std::abs(tick.time - state.note.endMilliseconds) >= 2.0) {
                continue;
            }
            const auto flick = std::lower_bound(flickTimes.cbegin(), flickTimes.cend(), tick.time - 2.0);
            if (flick != flickTimes.cend() && std::abs(tick.time - *flick) < 2.0) {
                tick.type = 4;
            }
        }
    }
}

void PlaytestSession::advance(const double chartTime, const double monotonicTime) {
    advanceTo(chartTime, monotonicTime, true);
}

void PlaytestSession::advanceTo(const double chartTime, const double monotonicTime, const bool evaluateAtEnd) {
    if (!std::isfinite(chartTime) || !std::isfinite(monotonicTime) || chartTime < m_time) {
        return;
    }
    const double previous = m_time;
    const double now = std::max(monotonicTime, m_monotonicTime);
    const double previousMonotonic = m_monotonicTime == 0.0 ? monotonicTime : m_monotonicTime;
    for (double time = std::floor(previous) + 1.0; time < chartTime; time += 1.0) {
        const double fraction = (time - previous) / (chartTime - previous);
        evaluate(time, previousMonotonic + (now - previousMonotonic) * fraction);
    }
    if (evaluateAtEnd) {
        evaluate(chartTime, now);
    }
    m_time = chartTime;
    m_monotonicTime = now;
    if (chartTime > m_endTime + 2000.0 && m_result.processed < m_result.total) {
        for (auto& state : m_notes) {
            for (auto& tick : state.ticks) {
                if (!tick.finalized) {
                    commit(state, tick, tick.grade == PlaytestGrade::Pending ? PlaytestGrade::Miss : tick.grade, chartTime);
                }
            }
            state.active = false;
            state.consumed = true;
        }
    }
}

void PlaytestSession::press(const int lane, const double chartTime, const double monotonicTime, const double eventTime) {
    if (m_autoplay || lane < 0 || lane >= 6) {
        return;
    }
    if (m_held[lane]) {
        advance(chartTime, monotonicTime);
        return;
    }
    advanceTo(chartTime, monotonicTime, false);
    m_held[lane] = true;
    m_lanes[lane] = {true, false, eventTime < 0.0 ? monotonicTime : eventTime};
    evaluate(m_time, m_monotonicTime);
    m_lanes[lane].fresh = false;
    m_lanes[lane].consumed = false;
}

void PlaytestSession::release(const int lane, const double chartTime, const double monotonicTime, const double eventTime) {
    if (m_autoplay || lane < 0 || lane >= 6) {
        return;
    }
    advanceTo(chartTime, monotonicTime, false);
    m_held[lane] = false;
    m_lanes[lane] = {false, false, eventTime < 0.0 ? monotonicTime : eventTime};
    evaluate(m_time, m_monotonicTime);
}

void PlaytestSession::move(const double normalizedDelta, const double chartTime, const double monotonicTime,
    const double eventTime) {
    if (m_autoplay || !std::isfinite(normalizedDelta)) {
        return;
    }
    advance(chartTime, monotonicTime);
    const double timestamp = eventTime < 0.0 ? monotonicTime : eventTime;
    m_cursor += normalizedDelta;
    recordMotion(normalizedDelta, timestamp);
}

void PlaytestSession::recordMotion(const double delta, const double monotonicTime) {
    if (delta > 0.0) {
        m_leftMotion.clear();
        m_rightMotion.push_back({monotonicTime, delta});
    } else if (delta < 0.0) {
        m_rightMotion.clear();
        m_leftMotion.push_back({monotonicTime, -delta});
    }
}

void PlaytestSession::clearInput(const double monotonicTime) {
    m_held.fill(false);
    m_autoplayLaneEnds.fill(m_time);
    for (auto& lane : m_lanes) {
        lane = {false, false, monotonicTime};
    }
    m_leftMotion.clear();
    m_rightMotion.clear();
    for (auto& note : m_notes) {
        note.active = false;
    }
}

void PlaytestSession::setAutoplay(const bool enabled) {
    if (m_autoplay == enabled) {
        return;
    }
    clearInput(m_monotonicTime);
    m_groups.clear();
    m_activeSkyGroups.clear();
    m_contactSkyGroups.clear();
    m_autoplayStartTime = m_time;
    m_autoplay = enabled;
    if (enabled) {
        planAutoplayCursor();
    } else {
        m_autoplayCursor.clear();
    }
}

void PlaytestSession::expireMotion(const double time) {
    for (auto* motion : {&m_leftMotion, &m_rightMotion}) {
        while (!motion->empty() && time - motion->front().time > 0.05) {
            motion->pop_front();
        }
    }
}

double PlaytestSession::sweepLeft() const {
    double left = cursor();
    for (const auto& motion : m_rightMotion) {
        left -= motion.distance;
    }
    return std::clamp(left, 0.0, 1.0);
}

double PlaytestSession::sweepRight() const {
    double right = cursor();
    for (const auto& motion : m_leftMotion) {
        right += motion.distance;
    }
    return std::clamp(right, 0.0, 1.0);
}

double PlaytestSession::coordinate(const double time) const {
    if (time < 0.0) {
        return time / 1000.0;
    }
    const auto end = std::upper_bound(m_scrollPoints.cbegin(), m_scrollPoints.cend(), time,
        [](const double value, const ScrollPoint& point) { return value < point.time; });
    const ScrollPoint& point = *std::prev(end);
    return (point.coordinate + (time - point.time) * point.speed) / 1000.0;
}

void PlaytestSession::evaluate(const double time, const double monotonicTime) {
    expireMotion(monotonicTime);
    if (m_autoplay) {
        evaluateAutoplay(time, monotonicTime);
        return;
    }
    const double scroll = coordinate(time);
    m_activeSkyGroups.clear();
    m_contactSkyGroups.clear();
    const auto& skyIndexes = m_sides[static_cast<int>(FloorSide::Sky)];
    for (qsizetype offset = m_firstPending[static_cast<int>(FloorSide::Sky)]; offset < skyIndexes.size(); ++offset) {
        const auto& state = m_notes[skyIndexes[offset]];
        if (state.note.startMilliseconds > time) {
            break;
        }
        if (state.note.kind != NoteKind::Sky || state.note.endMilliseconds < time) {
            continue;
        }
        const auto id = state.note.groupId;
        const bool contact = skyContact(state.note, time, time - state.note.startMilliseconds <= 1.0);
        m_activeSkyGroups[id] = true;
        m_contactSkyGroups[id] = m_contactSkyGroups.value(id) || contact;
        auto& group = m_groups[id];
        if (!group.initialized) {
            group.initialized = true;
            group.acquired = contact;
            group.lastUpdate = std::max<double>(state.note.startMilliseconds, m_time);
        }
    }
    for (auto group = m_groups.begin(); group != m_groups.end(); ++group) {
        updateSky(group.value(), time, m_contactSkyGroups.value(group.key()));
        if (m_contactSkyGroups.value(group.key()) && group->lockout == 0.0) {
            group->acquired = true;
        }
    }
    for (int side = 0; side < 4; ++side) {
        const auto& indexes = m_sides[side];
        auto& first = m_firstPending[side];
        while (first < indexes.size() && m_notes[indexes[first]].consumed) {
            ++first;
        }
        for (qsizetype offset = first; offset < indexes.size(); ++offset) {
            auto& state = m_notes[indexes[offset]];
            if (state.consumed) {
                continue;
            }
            if (state.note.startMilliseconds - time >= 1000.0
                && state.startCoordinate > scroll + 2.0) {
                break;
            }
            switch (state.note.kind) {
            case NoteKind::Tap:
            case NoteKind::Hold: evaluateFloor(state, time, monotonicTime); break;
            case NoteKind::Flick: evaluateFlick(state, time); break;
            case NoteKind::Sky: evaluateSky(state, time); break;
            default: break;
            }
            while (state.nextTick < state.ticks.size() && state.ticks[state.nextTick].finalized) {
                ++state.nextTick;
            }
            state.consumed = state.nextTick == state.ticks.size();
            if (state.consumed) {
                state.active = false;
            }
        }
    }
    for (auto group = m_groups.begin(); group != m_groups.end();) {
        if (m_skyGroupEnds.value(group.key()) < time) {
            group = m_groups.erase(group);
            continue;
        }
        updateSky(group.value(), time, m_activeSkyGroups.value(group.key()));
        ++group;
    }
}

void PlaytestSession::planAutoplayCursor() {
    struct Sample {
        double time;
        double left = 0.0;
        double right = 1.0;
        double position = 0.5;
        double midpoint = 0.5;
        bool constrained = false;
        bool fixed = false;
    };
    struct Stroke {
        double time;
        double center;
        double direction;
        double amplitude;
        double halfDuration = 40.0;
        double left = 0.0;
        double right = 1.0;
        int layers = 1;
        bool aligned = true;
        double headBias = 0.8;
    };
    QVector<Stroke> strokes;
    QVector<double> times{m_time};
    double end = m_time;
    for (const auto& state : m_notes) {
        const auto& note = state.note;
        if (note.kind == NoteKind::Sky && note.endMilliseconds >= m_time) {
            times.append(std::max<double>(m_time, note.startMilliseconds));
            times.append(note.endMilliseconds);
            end = std::max<double>(end, note.endMilliseconds);
        } else if (note.kind == NoteKind::Flick && note.startMilliseconds >= m_time) {
            const bool right = (note.auxiliary & 0x400U) != 0 || (note.auxiliary & 0x1000U) == 0;
            const double direction = right ? 1.0 : -1.0;
            const double leftEdge = note.startX - note.startWidth * 0.5;
            const double rightEdge = note.startX + note.startWidth * 0.5;
            if (!strokes.isEmpty() && strokes.last().time == note.startMilliseconds) {
                auto& stroke = strokes.last();
                stroke.left = std::min(stroke.left, leftEdge);
                stroke.right = std::max(stroke.right, rightEdge);
                stroke.aligned = stroke.aligned && stroke.direction == direction;
                ++stroke.layers;
                continue;
            }
            strokes.append({static_cast<double>(note.startMilliseconds),
                std::clamp(note.startX, 0.0, 1.0), direction,
                std::max(0.002, note.startWidth * 0.5), 40.0, leftEdge, rightEdge});
        }
    }
    m_autoplayCursor.clear();
    if (end == m_time && strokes.isEmpty()) {
        m_autoplayCursor.append({m_time, cursor()});
        return;
    }
    for (qsizetype index = 0; index < strokes.size(); ++index) {
        auto& stroke = strokes[index];
        if (!stroke.aligned) {
            stroke.left = stroke.center - stroke.amplitude;
            stroke.right = stroke.center + stroke.amplitude;
        }
        stroke.left = std::clamp(stroke.left, 0.0, 1.0);
        stroke.right = std::clamp(stroke.right, stroke.left, 1.0);
        stroke.center = (stroke.left + stroke.right) * 0.5;
        stroke.amplitude = (stroke.right - stroke.left) * 0.5;
        if (stroke.layers > 1 && stroke.aligned) {
            stroke.headBias = 0.0;
        }
        for (const qsizetype neighbor : {index - 1, index + 1}) {
            if (neighbor >= 0 && neighbor < strokes.size()) {
                stroke.halfDuration = std::min(stroke.halfDuration,
                    std::abs(strokes[neighbor].time - stroke.time) * 0.3);
            }
        }
        times.append(std::max(m_time, stroke.time - stroke.halfDuration * (1.0 + stroke.headBias)));
        times.append(stroke.time);
        times.append(stroke.time + stroke.halfDuration * (1.0 - stroke.headBias));
        end = std::max(end, stroke.time + stroke.halfDuration * (1.0 - stroke.headBias) + 120.0);
    }
    end = std::max(end, m_endTime);
    times.append(end);
    for (double time = m_time + kCursorSampleInterval; time < end; time += kCursorSampleInterval) {
        times.append(time);
    }
    std::sort(times.begin(), times.end());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    QVector<Sample> samples;
    samples.reserve(times.size());
    QVector<int> activeFields;
    const auto& skyIndexes = m_sides[static_cast<int>(FloorSide::Sky)];
    qsizetype nextSky = 0;
    int primary = -1;
    double previous = cursor();
    for (const double time : times) {
        while (nextSky < skyIndexes.size() && m_notes[skyIndexes[nextSky]].note.startMilliseconds <= time) {
            const int index = skyIndexes[nextSky++];
            const auto& note = m_notes[index].note;
            if (note.kind == NoteKind::Sky && note.endMilliseconds >= time) {
                activeFields.append(index);
            }
        }
        activeFields.removeIf([&](const int index) { return m_notes[index].note.endMilliseconds < time; });
        Sample sample{time};
        if (!activeFields.isEmpty()) {
            if (!activeFields.contains(primary)) {
                primary = *std::min_element(activeFields.cbegin(), activeFields.cend(), [&](const int a, const int b) {
                    const auto [al, ar] = skyBounds(m_notes[a].note, time);
                    const auto [bl, br] = skyBounds(m_notes[b].note, time);
                    return std::abs(std::clamp(previous, al, std::max(al, ar)) - previous)
                        < std::abs(std::clamp(previous, bl, std::max(bl, br)) - previous);
                });
            }
            const auto [left, right] = skyBounds(m_notes[primary].note, time);
            sample.left = std::clamp(left - kSkyContactTolerance, 0.0, 1.0);
            sample.right = std::clamp(right + kSkyContactTolerance, sample.left, 1.0);
            double sharedLeft = 0.0;
            double sharedRight = 1.0;
            for (const int index : activeFields) {
                const auto [otherLeft, otherRight] = skyBounds(m_notes[index].note, time);
                sharedLeft = std::max(sharedLeft, otherLeft);
                sharedRight = std::min(sharedRight, otherRight);
                const double contactLeft = std::max(sample.left, otherLeft - kSkyContactTolerance);
                const double contactRight = std::min(sample.right, otherRight + kSkyContactTolerance);
                if (contactLeft <= contactRight) {
                    sample.left = contactLeft;
                    sample.right = contactRight;
                }
            }
            if (sharedLeft <= sharedRight) {
                sample.left = sharedLeft;
                sample.right = sharedRight;
            } else if (sharedLeft - kSkyContactTolerance <= sharedRight + kSkyContactTolerance) {
                sample.left = std::clamp(sharedLeft - kSkyContactTolerance, 0.0, 1.0);
                sample.right = std::clamp(sharedRight + kSkyContactTolerance, sample.left, 1.0);
            }
            sample.position = (sample.left + sample.right) * 0.5;
            sample.midpoint = sample.position;
            sample.constrained = true;
            previous = sample.position;
        }
        samples.append(sample);
    }
    for (const auto& stroke : strokes) {
        auto first = std::lower_bound(samples.begin(), samples.end(), stroke.time - stroke.halfDuration * (1.0 + stroke.headBias),
            [](const Sample& sample, const double time) { return sample.time < time; });
        auto last = std::upper_bound(first, samples.end(), stroke.time + stroke.halfDuration * (1.0 - stroke.headBias),
            [](const double time, const Sample& sample) { return time < sample.time; });
        if (first == last) {
            continue;
        }
        const auto& end = *std::prev(last);
        const double tail = std::clamp(stroke.direction > 0.0 ? stroke.left : stroke.right, first->left, first->right);
        const double head = std::clamp(stroke.direction > 0.0 ? stroke.right : stroke.left, end.left, end.right);
        for (auto sample = first; sample != last; ++sample) {
            const double progress = ((sample->time - stroke.time) / stroke.halfDuration + stroke.headBias + 1.0) * 0.5;
            sample->position = std::clamp(std::lerp(tail, head, progress), sample->left, sample->right);
            sample->left = sample->right = sample->position;
            sample->constrained = sample->fixed = true;
        }
    }
    samples.first().position = samples.first().left = samples.first().right = cursor();
    samples.first().constrained = samples.first().fixed = true;
    QVector<std::pair<double, double>> transitions;
    for (qsizetype index = 1; index < samples.size(); ++index) {
        const auto& a = samples[index - 1];
        const auto& b = samples[index];
        const double distance = std::max(a.left - b.right, b.left - a.right);
        if (a.constrained && b.constrained && !(a.fixed && b.fixed) && distance > kSkyContactTolerance) {
            transitions.append({(a.time + b.time) * 0.5, std::clamp(distance / 0.003, 40.0, 120.0)});
        }
    }
    for (const auto& [time, radius] : transitions) {
        auto first = std::lower_bound(samples.begin(), samples.end(), time - radius,
            [](const Sample& sample, const double value) { return sample.time < value; });
        for (auto sample = first; sample != samples.end() && sample->time < time + radius; ++sample) {
            if (!sample->fixed) {
                sample->left = 0.0;
                sample->right = 1.0;
            }
        }
    }
    const auto limitReach = [](Sample& point, const Sample& neighbor) {
        const double reach = std::abs(point.time - neighbor.time) * 0.0025;
        const double left = std::max(point.left, neighbor.left - reach);
        const double right = std::min(point.right, neighbor.right + reach);
        if (!point.fixed && left <= right) {
            point.left = left;
            point.right = right;
        }
    };
    for (int pass = 0; pass < 2; ++pass) {
        for (qsizetype index = 1; index < samples.size(); ++index) {
            limitReach(samples[index], samples[index - 1]);
        }
        for (qsizetype index = samples.size() - 2; index >= 0; --index) {
            limitReach(samples[index], samples[index + 1]);
        }
    }
    qsizetype anchor = 0;
    for (qsizetype index = 1; index < samples.size(); ++index) {
        if (!samples[index].constrained && index + 1 < samples.size()) {
            continue;
        }
        if (!samples[index].constrained) {
            samples[index].position = samples[anchor].position;
        }
        for (qsizetype gap = anchor + 1; gap < index; ++gap) {
            const double progress = (samples[gap].time - samples[anchor].time)
                / (samples[index].time - samples[anchor].time);
            samples[gap].position = std::lerp(samples[anchor].position, samples[index].position, progress);
        }
        anchor = index;
    }
    const auto smooth = [](Sample& point, const Sample& neighbor) {
        const double dt = std::abs(point.time - neighbor.time);
        const double weight = -std::expm1(-dt / 80.0);
        double position = std::lerp(neighbor.position, point.position, weight);
        if (point.constrained && !point.fixed) {
            const double bias = -std::expm1(-dt / 320.0);
            position = std::lerp(position, point.midpoint, bias);
        }
        point.position = std::clamp(position, point.left, point.right);
    };
    for (int pass = 0; pass < 4; ++pass) {
        for (qsizetype index = 1; index < samples.size(); ++index) {
            smooth(samples[index], samples[index - 1]);
        }
        for (qsizetype index = samples.size() - 2; index >= 0; --index) {
            smooth(samples[index], samples[index + 1]);
        }
    }
    m_autoplayCursor.reserve(samples.size());
    for (qsizetype index = 0; index < samples.size(); ++index) {
        const auto& point = samples[index];
        double velocity = 0.0;
        if (index > 0 && index + 1 < samples.size()) {
            const auto& before = samples[index - 1];
            const auto& after = samples[index + 1];
            const double dtBefore = point.time - before.time;
            const double dtAfter = after.time - point.time;
            const double incoming = (point.position - before.position) / dtBefore;
            const double outgoing = (after.position - point.position) / dtAfter;
            if (incoming * outgoing > 0.0) {
                const double w1 = 2.0 * dtAfter + dtBefore;
                const double w2 = dtAfter + 2.0 * dtBefore;
                velocity = (w1 + w2) / (w1 / incoming + w2 / outgoing);
            }
        }
        m_autoplayCursor.append({point.time, point.position, velocity});
    }
}

double PlaytestSession::autoplayCursor(const double time) const {
    if (m_autoplayCursor.isEmpty()) {
        return cursor();
    }
    const auto next = std::upper_bound(m_autoplayCursor.cbegin(), m_autoplayCursor.cend(), time,
        [](const double value, const CursorPoint& point) { return value < point.time; });
    if (next == m_autoplayCursor.cbegin()) {
        return next->position;
    }
    if (next == m_autoplayCursor.cend()) {
        return m_autoplayCursor.last().position;
    }
    const auto& from = *std::prev(next);
    const auto& to = *next;
    const double duration = to.time - from.time;
    const double p = (time - from.time) / duration;
    const double p2 = p * p;
    const double p3 = p2 * p;
    return std::clamp((2.0 * p3 - 3.0 * p2 + 1.0) * from.position
        + (p3 - 2.0 * p2 + p) * duration * from.velocity
        + (-2.0 * p3 + 3.0 * p2) * to.position
        + (p3 - p2) * duration * to.velocity, 0.0, 1.0);
}

void PlaytestSession::evaluateAutoplay(const double time, const double monotonicTime) {
    const double position = autoplayCursor(time);
    recordMotion(position - cursor(), monotonicTime);
    m_cursor = position;
    for (int side = 0; side < 4; ++side) {
        const auto& indexes = m_sides[side];
        auto& first = m_firstPending[side];
        while (first < indexes.size() && m_notes[indexes[first]].consumed) {
            ++first;
        }
        for (qsizetype offset = first; offset < indexes.size(); ++offset) {
            auto& state = m_notes[indexes[offset]];
            if (state.note.startMilliseconds > time) {
                break;
            }
            if (state.consumed) {
                continue;
            }
            const bool sustained = state.note.kind == NoteKind::Hold || state.note.kind == NoteKind::Sky;
            state.active = sustained && time < state.note.endMilliseconds
                && (state.note.kind != NoteKind::Sky
                    || skyContact(state.note, time, time - state.note.startMilliseconds <= 1.0));
            state.recontact = false;
            if (state.active) {
                state.successfulUntil = std::max(state.successfulUntil, time);
                if (state.note.kind == NoteKind::Hold) {
                    for (int lane = state.firstLane; lane <= state.lastLane; ++lane) {
                        m_autoplayLaneEnds[lane] = std::max(m_autoplayLaneEnds[lane],
                            static_cast<double>(state.note.endMilliseconds));
                    }
                }
            }
            while (state.nextTick < state.ticks.size()) {
                auto& tick = state.ticks[state.nextTick];
                if (tick.time > time) {
                    break;
                }
                if (!tick.finalized) {
                    if (tick.time >= m_autoplayStartTime) {
                        if (state.note.kind == NoteKind::Tap) {
                            for (int lane = state.firstLane; lane <= state.lastLane; ++lane) {
                                m_autoplayLaneEnds[lane] = std::max(m_autoplayLaneEnds[lane], tick.time + 50.0);
                            }
                        }
                        commit(state, tick, PlaytestGrade::ExactPlus, tick.time);
                    } else {
                        commit(state, tick, tick.grade == PlaytestGrade::Pending ? PlaytestGrade::Miss : tick.grade, time);
                    }
                }
                ++state.nextTick;
            }
            state.consumed = state.nextTick == state.ticks.size();
            if (state.consumed) {
                state.active = false;
            }
        }
    }
    for (std::size_t lane = 0; lane < m_held.size(); ++lane) {
        m_held[lane] = time < m_autoplayLaneEnds[lane];
    }
}

void PlaytestSession::evaluateFloor(PlaytestNoteState& state, const double time, const double monotonicTime) {
    bool held = false;
    for (int lane = state.firstLane; lane <= state.lastLane; ++lane) {
        held = held || m_held[lane];
    }
    const bool started = !state.ticks.isEmpty() && (state.ticks.front().finalized || state.ticks.front().type != 1);
    const bool contact = started && held && time >= state.note.startMilliseconds - 100.0
        && time <= state.note.endMilliseconds + 100.0;
    if (state.active && !contact && state.note.endMilliseconds - time > 50.0) {
        state.recontact = true;
    }
    state.active = state.note.kind == NoteKind::Hold && contact;
    for (qsizetype index = state.nextTick; index < state.ticks.size(); ++index) {
        auto& tick = state.ticks[index];
        if (tick.finalized) {
            continue;
        }
        const double delta = tick.time - time;
        if (delta < -100.0) {
            commit(state, tick, tick.grade == PlaytestGrade::Pending ? PlaytestGrade::Miss : tick.grade, time);
            continue;
        }
        if (delta > (tick.type == 1 ? 150.0 : 100.0)) {
            break;
        }
        if (tick.type == 1) {
            for (int lane = state.firstLane; lane <= state.lastLane; ++lane) {
                if (m_lanes[lane].fresh && !m_lanes[lane].consumed) {
                    m_lanes[lane].consumed = true;
                    commit(state, tick, timingGrade(std::abs(delta), delta), time);
                    break;
                }
            }
        } else {
            double candidate = -1000.0;
            for (int lane = state.firstLane; lane <= state.lastLane; ++lane) {
                const double value = m_held[lane] ? delta
                    : delta + std::max(0.0, monotonicTime - m_lanes[lane].transition) * 1000.0;
                if (std::abs(value) < std::abs(candidate)) {
                    candidate = value;
                }
            }
            PlaytestGrade grade = timingGrade(std::abs(candidate), delta);
            if (state.recontact && held && index + 1 < state.ticks.size() && grade != PlaytestGrade::Miss) {
                tick.grade = PlaytestGrade::NearEarly;
                if (delta <= 0.0) {
                    commit(state, tick, PlaytestGrade::NearEarly, time);
                    state.recontact = false;
                }
            } else if (grade != PlaytestGrade::Miss || delta < 0.0) {
                improve(state, tick, grade, delta);
            }
        }
    }
}

void PlaytestSession::improve(PlaytestNoteState& state, PlaytestTick& tick,
    const PlaytestGrade grade, const double delta) {
    const auto mask = delta >= 0.0 ? kEarlyMask : kLateMask;
    const bool accepted = allowed(mask, grade, tick.grade);
    if (accepted) {
        tick.grade = grade;
    }
    if ((tick.grade == PlaytestGrade::ExactPlus && (tick.type != 5 || delta <= 0.0))
        || (delta < 0.0 && (!accepted || tick.grade == PlaytestGrade::Miss))) {
        commit(state, tick, tick.grade == PlaytestGrade::Pending ? PlaytestGrade::Miss : tick.grade, tick.time - delta);
    }
}

void PlaytestSession::evaluateFlick(PlaytestNoteState& state, const double time) {
    auto& tick = state.ticks.front();
    const double delta = tick.time - time;
    if (delta < -200.0) {
        commit(state, tick, tick.grade == PlaytestGrade::Pending ? PlaytestGrade::Miss : tick.grade, time);
        return;
    }
    if (delta > 200.0) {
        return;
    }
    const bool right = (state.note.auxiliary & 0x400U) != 0 || (state.note.auxiliary & 0x1000U) == 0;
    const double leftEdge = state.note.startX - state.note.startWidth * 0.5;
    const double rightEdge = state.note.startX + state.note.startWidth * 0.5;
    const bool overlap = sweepRight() - sweepLeft() > 0.001
        && sweepRight() >= leftEdge && sweepLeft() <= rightEdge;
    const bool motion = right ? !m_rightMotion.empty() : !m_leftMotion.empty();
    if (overlap && motion) {
        improve(state, tick, timingGrade(std::abs(delta), delta, 2.0), delta);
    } else if (delta < 0.0 && tick.grade != PlaytestGrade::Pending) {
        commit(state, tick, tick.grade, time);
    }
}

bool PlaytestSession::skyContact(const ChartNote& note, const double time, const bool swept) const {
    const auto [left, right] = skyBounds(note, time);
    return (swept ? sweepRight() : cursor()) >= left - kSkyContactTolerance
        && (swept ? sweepLeft() : cursor()) <= right + kSkyContactTolerance;
}

void PlaytestSession::updateSky(SkyGroup& group, const double time, const bool contact) {
    double dt = time - group.lastUpdate;
    if (dt < 0.0) {
        return;
    }
    if (group.lockout > 0.0) {
        const double lockedTime = std::min(dt, group.lockout);
        group.lockout -= lockedTime;
        dt -= lockedTime;
        if (group.lockout == 0.0) {
            group.credit = 300.0;
        }
    }
    if (group.lockout == 0.0 && contact) {
        group.credit = std::clamp(group.credit + dt, 0.0, 300.0);
    } else {
        group.credit = std::clamp(group.credit - 2.0 * dt, 0.0, 300.0);
        if (group.credit == 0.0 && group.lockout == 0.0) {
            group.acquired = false;
            group.lockout = 500.0;
        }
    }
    group.lastUpdate = time;
}

void PlaytestSession::evaluateSky(PlaytestNoteState& state, const double time) {
    if (time < state.note.startMilliseconds) {
        return;
    }
    auto& group = m_groups[state.note.groupId];
    const bool activeTime = time <= state.note.endMilliseconds;
    const bool contact = activeTime && skyContact(state.note, time, time - state.note.startMilliseconds <= 1.0);
    if (!group.initialized) {
        group.initialized = true;
        group.acquired = contact;
        group.lastUpdate = std::max<double>(state.note.startMilliseconds, m_time);
    }
    for (qsizetype index = state.nextTick; index < state.ticks.size(); ++index) {
        auto& tick = state.ticks[index];
        if (tick.time > time) {
            break;
        }
        if (tick.finalized) {
            continue;
        }
        updateSky(group, std::max(tick.time, group.lastUpdate), skyContact(state.note, tick.time, false));
        if (group.acquired && group.lockout == 0.0) {
            commit(state, tick, PlaytestGrade::ExactPlus, time);
        } else if (group.lockout > 0.0 || !activeTime) {
            commit(state, tick, PlaytestGrade::Miss, time);
        }
    }
    state.active = activeTime && group.acquired && group.lockout == 0.0 && contact;
}

void PlaytestSession::commit(PlaytestNoteState& state, PlaytestTick& tick, const PlaytestGrade grade, const double time) {
    if (tick.finalized || grade == PlaytestGrade::Pending) {
        return;
    }
    tick.grade = grade;
    tick.finalized = true;
    state.missed = state.missed || grade == PlaytestGrade::Miss;
    if (grade != PlaytestGrade::Miss) {
        state.successfulUntil = std::max(state.successfulUntil, tick.time);
    }
    const int index = static_cast<int>(grade);
    ++m_result.counts[index];
    ++m_result.countsByKind[static_cast<int>(state.note.kind)][index];
    ++m_result.processed;
    m_result.combo = grade == PlaytestGrade::Miss ? 0 : m_result.combo + 1;
    m_result.maxCombo = std::max(m_result.combo, m_result.maxCombo);
    const int exact = m_result.counts[4] + m_result.counts[5] + m_result.counts[6];
    const int near = m_result.counts[2] + m_result.counts[3];
    const int plus = m_result.counts[6];
    if (m_result.total > 0) {
        const double raw = exact * (100000000.0 / m_result.total)
            + near * (50000000.0 / m_result.total) + plus;
        m_result.score = std::min<qint64>(static_cast<qint64>(std::ceil(raw)), 100000000LL + m_result.total);
        if (m_result.processed == m_result.total && near == 0 && m_result.counts[1] == 0) {
            m_result.score = 100000000LL + plus;
        }
    }
    double position = (state.firstLane + state.lastLane) * 0.5 / 5.0;
    if (state.note.kind == NoteKind::Sky) {
        const auto [left, right] = skyBounds(state.note, time);
        position = (left + right) * 0.5;
    } else if (state.note.kind == NoteKind::Flick) {
        position = grade == PlaytestGrade::Miss ? state.note.startX : cursor();
    }
    m_judgements.append({state.note.id, tick.id, state.note.kind, state.note.side, tick.type, grade,
        std::clamp(position, 0.0, 1.0), time});
}

bool PlaytestSession::complete() const {
    const double end = m_autoplay && !m_autoplayCursor.isEmpty()
        ? std::max(m_endTime, m_autoplayCursor.last().time) : m_endTime;
    return m_time >= end && m_result.processed == m_result.total;
}

QVector<PlaytestJudgement> PlaytestSession::takeJudgements() {
    return std::exchange(m_judgements, {});
}

} // namespace infalsus
