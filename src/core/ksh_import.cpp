#include "core/ksh_import.h"

#include "core/field_easing.h"
#include "core/timing_grid.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QHash>
#include <QtCore/QStringConverter>
#include <QtCore/QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <map>
#include <numeric>
#include <optional>
#include <tuple>
#include <utility>

namespace infalsus {
namespace {

constexpr QLatin1StringView kLaserPositions("0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmno");
constexpr QLatin1StringView kMeasureSeparator("--");
constexpr double kMillisecondsPerMinute = 60000.0;
constexpr double kQuarterNotesPerWholeNote = 4.0;
constexpr double kMaximumSlamBeats = kQuarterNotesPerWholeNote / 32.0;
constexpr double kBeatTolerance = 1e-9;
constexpr double kCentralLaneWidth = 0.25;
constexpr double kMinimumKshFieldWidth = kMinimumKshFieldWidthLanes / kMaximumKshFieldWidthLanes;
constexpr qint64 kMinimumSustainedDuration = 2;
constexpr qsizetype kFirstLaserColumn = 8;
constexpr std::size_t kLaserCount = 2;
constexpr double kLaserPositionStep = 1.0 / (kLaserPositions.size() - 1);
constexpr double kCurveFitTolerance = kLaserPositionStep;
constexpr qsizetype kMinimumCurveSegments = 3;
constexpr qsizetype kMaximumCurveSegments = 24;
constexpr int kCurveSamples = 8;
constexpr int kRootIterations = 24;

constexpr std::array kFieldEasings{FieldEasing::Linear, FieldEasing::SineIn, FieldEasing::SineOut};

using Header = QHash<QString, QString>;

struct Meter {
    int numerator = 4;
    int denominator = 4;
};

struct TimingMarker {
    TimingPoint point;
    double beat = 0.0;
    double milliseconds = 0.0;
};

struct CurveControl {
    double time;
    double position;

    [[nodiscard]] double progressAt(const double progress) const {
        if (time == position || progress <= 0.0 || progress >= 1.0) {
            return progress;
        }
        const double parameter = progress / (time + std::sqrt(time * time + (1.0 - 2.0 * time) * progress));

        return parameter * (2.0 * (1.0 - parameter) * position + parameter);
    }
};

struct TimedRow {
    QStringView notes;
    std::array<std::optional<CurveControl>, kLaserCount> laserCurves;
    double beat = 0.0;
    double milliseconds = 0.0;
};

struct TimedChart {
    QVector<TimedRow> rows;
    QVector<TimingMarker> timing;
    double endMilliseconds = 0.0;
};

struct LaserSegment {
    double startBeat;
    double endBeat;
    double startX;
    double endX;
    std::optional<CurveControl> curve;
    FieldEasing easing = FieldEasing::Linear;

    [[nodiscard]] bool curved() const {
        return easing != FieldEasing::Linear || (curve && curve->time != curve->position);
    }

    [[nodiscard]] double positionAt(const double beat) const {
        const double progress = std::clamp((beat - startBeat) / (endBeat - startBeat), 0.0, 1.0);

        return std::lerp(startX, endX, curve ? curve->progressAt(progress) : fieldEase(easing, progress));
    }
};

struct LaserTrack {
    QVector<LaserSegment> segments;
    QVector<ChartNote> flicks;
};

struct LaserPoint {
    std::optional<CurveControl> curve;
    double sourceBeat;
    double startBeat;
    double milliseconds;
    double position;
};

struct Span {
    double center;
    double width;
};

struct GroundLane {
    FloorSide side;
    qsizetype column;
    double x;
    QChar tap;
    QChar hold;
};

struct ButtonNote {
    const GroundLane* lane;
    NoteKind kind;
    double start;
    double end;
};

constexpr std::array kGroundLanes{
    GroundLane{FloorSide::Central, 0, kCentralLaneWidth, u'1', u'2'},
    GroundLane{FloorSide::Central, 1, kCentralLaneWidth * 2, u'1', u'2'},
    GroundLane{FloorSide::Central, 2, kCentralLaneWidth * 3, u'1', u'2'},
    GroundLane{FloorSide::Central, 3, kCentralLaneWidth * 4, u'1', u'2'},
    GroundLane{FloorSide::LeftOuter, 5, 0.0, u'2', u'1'},
    GroundLane{FloorSide::RightOuter, 6, 0.0, u'2', u'1'},
};

[[nodiscard]] QString decodeText(const QByteArray& bytes) {
    QStringDecoder utf8(QStringDecoder::Utf8);
    const QString text = utf8(bytes);
    if (bytes.startsWith("\xEF\xBB\xBF") || !utf8.hasError()) {
        return text;
    }
    QStringDecoder system(QStringDecoder::System);

    return system(bytes);
}

[[nodiscard]] double readTempo(const QStringView value) {
    const double tempo = value.toDouble();

    return std::isfinite(tempo) && tempo >= 0.001 && tempo <= 65535.0 ? tempo : 0.0;
}

[[nodiscard]] Meter readMeter(const QStringView value, const Meter fallback) {
    const qsizetype separator = value.indexOf(u'/');
    if (separator < 0) {
        return fallback;
    }
    const int numerator = value.first(separator).toInt();
    const int denominator = value.sliced(separator + 1).toInt();

    return numerator > 0 && denominator > 0 ? Meter{numerator, denominator} : fallback;
}

[[nodiscard]] bool isNoteRow(const QStringView line) {
    return line.size() >= 10 && line[4] == u'|' && line[7] == u'|'
        && std::ranges::all_of(line.first(4), [](const QChar value) { return value >= u'0' && value <= u'2'; });
}

[[nodiscard]] std::optional<CurveControl> readCurveControl(const QStringView option) {
    const qsizetype separator = option.indexOf(u';');
    if (separator < 0) {
        return std::nullopt;
    }
    bool validTime = false;
    bool validPosition = false;
    const double time = option.first(separator).toDouble(&validTime);
    const double position = option.sliced(separator + 1).toDouble(&validPosition);
    if (!validTime || !validPosition || !(time >= 0.0 && time <= 1.0 && position >= 0.0 && position <= 1.0)) {
        return std::nullopt;
    }

    return CurveControl{time, position};
}

[[nodiscard]] TimedChart readTiming(const QStringList& lines, const qsizetype bodyStart, const Header& header) {
    TimedChart chart;
    std::array<std::optional<CurveControl>, kLaserCount> laserCurves;
    Meter meter = readMeter(header.value(QStringLiteral("beat")), {});
    Meter nextMeter = meter;
    double tempo = readTempo(header.value(QStringLiteral("t")));
    double beat = 0.0;
    double milliseconds = header.value(QStringLiteral("o")).toInt();
    qsizetype measureStart = bodyStart;

    for (qsizetype measureEnd = bodyStart; measureEnd <= lines.size(); ++measureEnd) {
        if (measureEnd < lines.size() && lines.at(measureEnd).trimmed() != kMeasureSeparator) {
            continue;
        }
        meter = nextMeter;
        const auto first = lines.cbegin() + measureStart;
        const auto last = lines.cbegin() + measureEnd;
        const auto rowCount = std::count_if(first, last,
            [](const QString& line) { return isNoteRow(QStringView(line).trimmed()); });
        bool measureStarted = false;
        double rowBeats = 0.0;
        for (auto entry = first; entry != last; ++entry) {
            const QStringView line = QStringView(*entry).trimmed();
            if (line.startsWith(u"t=")) {
                const double change = readTempo(line.sliced(2));
                if (change > 0.0) {
                    tempo = change;
                }
            } else if (line.startsWith(u"beat=")) {
                nextMeter = readMeter(line.sliced(5), nextMeter);
                if (!measureStarted) {
                    meter = nextMeter;
                }
            } else if (line.startsWith(u"laser_l_curve=") || line.startsWith(u"laser_r_curve=")) {
                const std::size_t laser = line.startsWith(u"laser_l_curve=") ? 0 : 1;
                laserCurves[laser] = readCurveControl(line.sliced(line.indexOf(u'=') + 1));
            } else if (isNoteRow(line)) {
                if (!measureStarted) {
                    rowBeats = kQuarterNotesPerWholeNote * meter.numerator / meter.denominator / rowCount;
                    measureStarted = true;
                }
                const double bpm = tempo > 0.0 ? tempo : TimingPoint{}.beatsPerMinute;
                const TimingPoint point{qRound64(milliseconds), bpm, meter.numerator, meter.denominator};
                if (chart.timing.isEmpty() || chart.timing.back().point.beatsPerMinute != bpm
                    || chart.timing.back().point.timeSignatureNumerator != meter.numerator
                    || chart.timing.back().point.timeSignatureDenominator != meter.denominator) {
                    chart.timing.append({point, beat, milliseconds});
                }
                chart.rows.append({line, laserCurves, beat, milliseconds});
                laserCurves = {};
                beat += rowBeats;
                milliseconds += rowBeats * kMillisecondsPerMinute / bpm;
            }
        }
        measureStart = measureEnd + 1;
    }
    chart.endMilliseconds = milliseconds;

    return chart;
}

[[nodiscard]] double millisecondsAt(const TimedChart& chart, const double beat) {
    const auto next = std::upper_bound(chart.timing.cbegin(), chart.timing.cend(), beat,
        [](const double target, const TimingMarker& marker) { return target < marker.beat; });
    const auto& marker = next == chart.timing.cbegin() ? chart.timing.front() : *std::prev(next);

    return marker.milliseconds + (beat - marker.beat) * kMillisecondsPerMinute / marker.point.beatsPerMinute;
}

[[nodiscard]] QVector<TimingPoint> editorTiming(const TimedChart& source) {
    QVector<TimingPoint> points;
    points.reserve(source.timing.size());
    for (const TimingMarker& marker : source.timing) {
        const TimingPoint& point = marker.point;
        if (!points.isEmpty() && point.timeMilliseconds == points.back().timeMilliseconds) {
            points.back() = point;
        } else {
            points.append(point);
        }
    }

    return points;
}

[[nodiscard]] qint64 snapImportTime(const QVector<TimingPoint>& timing, const qint64 milliseconds) {
    const qint64 snapped = nearestValidDivisorTime(timing, std::max<qint64>(0, milliseconds));

    return snapped < 0 ? nextValidDivisorTime(timing, -1) : snapped;
}

[[nodiscard]] qint64 snapImportTimeAfter(const QVector<TimingPoint>& timing, const qint64 milliseconds,
    const qint64 earliest) {
    const qint64 snapped = snapImportTime(timing, milliseconds);

    return snapped >= earliest ? snapped : nextValidDivisorTime(timing, earliest - 1);
}

[[nodiscard]] ChartNote groundNote(const ButtonNote& note, const double width, const qint64 start, const qint64 end) {
    return {.startMilliseconds = start, .endMilliseconds = end,
        .startX = note.lane->x, .endX = note.lane->x,
        .startWidth = width, .endWidth = width, .kind = note.kind, .side = note.lane->side};
}

[[nodiscard]] QVector<ButtonNote> readButtons(const TimedChart& source) {
    QVector<ButtonNote> buttons;
    for (const GroundLane& lane : kGroundLanes) {
        std::optional<double> holdStart;
        const auto finishHold = [&](const double end) {
            if (holdStart && end > 0.0) {
                buttons.append({&lane, NoteKind::Hold, *holdStart, end});
            }
            holdStart.reset();
        };
        for (const TimedRow& row : source.rows) {
            const QChar symbol = row.notes[lane.column];
            const bool hold = symbol == lane.hold
                || (lane.side != FloorSide::Central && symbol != u'0' && symbol != lane.tap);
            if (hold) {
                if (!holdStart) {
                    holdStart = row.milliseconds;
                }
                continue;
            }
            finishHold(row.milliseconds);
            if (symbol == lane.tap && row.milliseconds >= 0.0) {
                buttons.append({&lane, NoteKind::Tap, row.milliseconds, row.milliseconds});
            }
        }
        finishHold(source.endMilliseconds);
    }

    return buttons;
}

void appendGroundNotes(const TimedChart& source, QVector<ChartNote>& notes, const QVector<TimingPoint>& timing) {
    auto buttons = readButtons(source);
    std::array<qint64, kGroundLanes.size()> laneAvailable{};
    std::sort(buttons.begin(), buttons.end(), [](const ButtonNote& first, const ButtonNote& second) {
        return std::tie(first.start, first.end, first.kind, first.lane->side, first.lane->x)
            < std::tie(second.start, second.end, second.kind, second.lane->side, second.lane->x);
    });

    for (auto first = buttons.cbegin(); first != buttons.cend();) {
        auto next = std::next(first);
        double width = 1.0;
        if (first->lane->side == FloorSide::Central) {
            width = kCentralLaneWidth;
            while (next != buttons.cend() && next->lane->side == FloorSide::Central
                && next->kind == first->kind && next->start == first->start && next->end == first->end
                && next->lane->x == first->lane->x + width) {
                width += kCentralLaneWidth;
                ++next;
            }
        }
        qint64 earliest = 0;
        for (auto button = first; button != next; ++button) {
            const auto lane = button->lane - kGroundLanes.data();
            earliest = std::max(earliest, laneAvailable[lane]);
        }
        const qint64 start = snapImportTimeAfter(timing, qRound64(first->start), earliest);
        const bool hold = first->kind == NoteKind::Hold;
        const qint64 end = hold
            ? snapImportTimeAfter(timing, qRound64(first->end), start + kMinimumSustainedDuration) : start;
        notes.append(groundNote(*first, width, start, end));
        const qint64 available = hold ? end : start + 1;
        for (auto button = first; button != next; ++button) {
            laneAvailable[button->lane - kGroundLanes.data()] = available;
        }
        first = next;
    }
}

[[nodiscard]] Span centeredSpan(const double position, const double width) {
    const double half = width * 0.5;

    return {std::clamp(position, half, 1.0 - half), width};
}

[[nodiscard]] ChartNote flickNote(const LaserPoint& before, const double after) {
    const Span span = centeredSpan(std::midpoint(before.position, after),
        std::max(kMinimumFlickWidth, std::abs(after - before.position)));
    const qint64 time = qRound64(before.milliseconds);

    return {.auxiliary = after > before.position ? kFlickRight : kFlickLeft,
        .startMilliseconds = time, .endMilliseconds = time,
        .startX = span.center, .endX = span.center, .startWidth = span.width, .endWidth = span.width,
        .kind = NoteKind::Flick, .side = FloorSide::Sky};
}

[[nodiscard]] LaserTrack readLaser(const TimedChart& source, const std::size_t laser) {
    LaserTrack track;
    std::optional<LaserPoint> previous;
    const qsizetype column = kFirstLaserColumn + static_cast<qsizetype>(laser);

    for (const TimedRow& row : source.rows) {
        const QChar symbol = row.notes[column];
        if (symbol == u'-') {
            previous.reset();
            continue;
        }
        const qsizetype step = kLaserPositions.indexOf(symbol);
        if (step < 0) {
            continue;
        }
        const double position = static_cast<double>(step) * kLaserPositionStep;
        LaserPoint current{row.laserCurves[laser], row.beat, row.beat, row.milliseconds, position};
        if (previous) {
            const bool slam = position != previous->position
                && row.beat - previous->sourceBeat <= kMaximumSlamBeats + kBeatTolerance;
            if (slam) {
                if (previous->milliseconds >= 0.0) {
                    track.flicks.append(flickNote(*previous, position));
                }
                if (previous->startBeat < previous->sourceBeat) {
                    track.segments.append({previous->startBeat, previous->sourceBeat, previous->position, previous->position});
                }
                current.startBeat = previous->sourceBeat;
                current.curve = previous->curve;
            } else {
                track.segments.append({previous->startBeat, row.beat, previous->position, position, previous->curve});
            }
        }
        previous = current;
    }

    return track;
}

enum class LaserFit { Linear, SineIn, SineOut, SineInOut };

[[nodiscard]] double fitProgress(const LaserFit fit, const double progress) {
    switch (fit) {
    case LaserFit::SineIn:
        return fieldEase(FieldEasing::SineIn, progress);
    case LaserFit::SineOut:
        return fieldEase(FieldEasing::SineOut, progress);
    case LaserFit::SineInOut:
        return 0.5 * (1.0 - std::cos(progress * std::numbers::pi));
    case LaserFit::Linear:
        return progress;
    }

    return progress;
}

[[nodiscard]] LaserFit fitLaserCurve(const QVector<LaserSegment>& segments, const qsizetype first,
    const qsizetype last) {
    constexpr std::array kFits{LaserFit::Linear, LaserFit::SineIn, LaserFit::SineOut, LaserFit::SineInOut};
    std::array<double, kFits.size()> squareErrors{};
    std::array<double, kFits.size()> maximumErrors{};
    const LaserSegment& start = segments[first];
    const LaserSegment& end = segments[last];
    const double duration = end.endBeat - start.startBeat;
    for (qsizetype index = first; index < last; ++index) {
        const auto& segment = segments[index];
        const double progress = (segment.endBeat - start.startBeat) / duration;
        for (std::size_t fit = 0; fit < kFits.size(); ++fit) {
            const double expected = std::lerp(start.startX, end.endX, fitProgress(kFits[fit], progress));
            const double error = std::abs(expected - segment.endX);
            maximumErrors[fit] = std::max(maximumErrors[fit], error);
            squareErrors[fit] += error * error;
        }
    }
    const double linearError = squareErrors[0];
    double bestError = linearError * 0.4;
    LaserFit best = LaserFit::Linear;
    if (maximumErrors[0] < kCurveFitTolerance) {
        return best;
    }
    for (std::size_t fit = 1; fit < kFits.size(); ++fit) {
        if (maximumErrors[fit] <= kCurveFitTolerance && squareErrors[fit] < bestError) {
            bestError = squareErrors[fit];
            best = kFits[fit];
        }
    }

    return best;
}

[[nodiscard]] bool isContinuousTurn(const LaserSegment& first, const LaserSegment& second) {
    return !first.curve && !second.curve && first.endBeat == second.startBeat && first.endX == second.startX
        && (first.endX - first.startX) * (second.endX - second.startX) > 0.0;
}

[[nodiscard]] QVector<LaserSegment> restoreLaserCurves(const QVector<LaserSegment>& segments) {
    QVector<LaserSegment> restored;
    restored.reserve(segments.size());
    for (qsizetype first = 0; first < segments.size();) {
        qsizetype last = first;
        while (last + 1 < segments.size() && last - first + 1 < kMaximumCurveSegments
            && isContinuousTurn(segments[last], segments[last + 1])) {
            ++last;
        }
        LaserFit fit = LaserFit::Linear;
        while (last - first + 1 >= kMinimumCurveSegments) {
            fit = fitLaserCurve(segments, first, last);
            if (fit != LaserFit::Linear) {
                break;
            }
            --last;
        }
        const LaserSegment& start = segments[first];
        if (fit == LaserFit::Linear) {
            restored.append(start);
            ++first;
            continue;
        }
        const LaserSegment& end = segments[last];
        if (fit == LaserFit::SineInOut) {
            const double middleBeat = std::midpoint(start.startBeat, end.endBeat);
            const double middleX = std::midpoint(start.startX, end.endX);
            restored.append({start.startBeat, middleBeat, start.startX, middleX, {}, FieldEasing::SineIn});
            restored.append({middleBeat, end.endBeat, middleX, end.endX, {}, FieldEasing::SineOut});
        } else {
            restored.append({start.startBeat, end.endBeat, start.startX, end.endX, {},
                fit == LaserFit::SineIn ? FieldEasing::SineIn : FieldEasing::SineOut});
        }
        first = last + 1;
    }

    return restored;
}

[[nodiscard]] const LaserSegment* segmentAt(const QVector<LaserSegment>& segments,
    qsizetype& index, const double beat) {
    while (index < segments.size() && segments[index].endBeat <= beat) {
        ++index;
    }

    return index < segments.size() && segments[index].startBeat <= beat ? &segments[index] : nullptr;
}

template<typename ValueAt>
[[nodiscard]] double findCrossing(const ValueAt& valueAt, double from, double to, const double threshold) {
    const bool startsBelow = valueAt(from) < threshold;
    for (int iteration = 0; iteration < kRootIterations; ++iteration) {
        const double middle = std::midpoint(from, to);
        if ((valueAt(middle) < threshold) == startsBelow) {
            from = middle;
        } else {
            to = middle;
        }
    }

    return std::midpoint(from, to);
}

template<typename ValueAt>
void addCrossings(QVector<double>& points, const double start, const double end, const ValueAt& valueAt,
    const double threshold, const bool curved) {
    const int samples = curved ? kCurveSamples : 1;
    double from = start;
    double startValue = valueAt(from);
    for (int sample = 1; sample <= samples; ++sample) {
        const double to = std::lerp(start, end, static_cast<double>(sample) / samples);
        const double endValue = valueAt(to);
        if ((startValue < threshold) != (endValue < threshold)) {
            const double crossing = curved && startValue != threshold && endValue != threshold
                ? findCrossing(valueAt, from, to, threshold)
                : std::lerp(from, to, (threshold - startValue) / (endValue - startValue));
            if (crossing > start + kBeatTolerance && crossing < end - kBeatTolerance) {
                points.append(crossing);
            }
        }
        from = to;
        startValue = endValue;
    }
}

[[nodiscard]] Span laserSpan(const LaserSegment& first, const LaserSegment* second,
    const double beat, const double singleWidth) {
    const double firstX = first.positionAt(beat);
    if (!second) {
        return centeredSpan(firstX, singleWidth);
    }
    const double secondX = second->positionAt(beat);

    return centeredSpan(std::midpoint(firstX, secondX), std::max(kMinimumKshFieldWidth, std::abs(firstX - secondX)));
}

[[nodiscard]] double edge(const Span& span, const bool right) {
    return span.center + (right ? 0.5 : -0.5) * span.width;
}

[[nodiscard]] double easingSlope(const FieldEasing easing, const double progress) {
    constexpr double kQuarterTurn = std::numbers::pi * 0.5;
    switch (easing) {
    case FieldEasing::SineIn:
        return kQuarterTurn * std::sin(progress * kQuarterTurn);
    case FieldEasing::SineOut:
        return kQuarterTurn * std::cos(progress * kQuarterTurn);
    case FieldEasing::Linear:
        return 1.0;
    }

    return 1.0;
}

[[nodiscard]] bool keepsMinimumWidth(const Span& from, const Span& to, const double minimumWidth,
    const FieldEasing left, const FieldEasing right) {
    if (left == right) {
        return true;
    }
    const double leftDelta = edge(to, false) - edge(from, false);
    const double rightDelta = edge(to, true) - edge(from, true);
    const auto widthSlope = [=](const double progress) {
        return rightDelta * easingSlope(right, progress) - leftDelta * easingSlope(left, progress);
    };
    if (widthSlope(0.0) >= 0.0 || widthSlope(1.0) <= 0.0) {
        return true;
    }
    const double minimumAt = findCrossing(widthSlope, 0.0, 1.0, 0.0);
    const double width = from.width + rightDelta * fieldEase(right, minimumAt) - leftDelta * fieldEase(left, minimumAt);

    return width + 1e-12 >= minimumWidth;
}

[[nodiscard]] std::uint32_t fitFieldEdges(const LaserSegment& first, const LaserSegment* second,
    const double start, const double end, const double fieldWidth, const Span& from, const Span& to) {
    if (!first.curved() && !(second && second->curved())) {
        return 0;
    }
    std::array<std::array<double, kFieldEasings.size()>, 2> errors{};
    for (int sample = 1; sample < kCurveSamples; ++sample) {
        const double progress = static_cast<double>(sample) / kCurveSamples;
        const Span actual = laserSpan(first, second, std::lerp(start, end, progress), fieldWidth);
        for (const bool right : {false, true}) {
            for (std::size_t index = 0; index < kFieldEasings.size(); ++index) {
                const double fitted = std::lerp(edge(from, right), edge(to, right),
                    fieldEase(kFieldEasings[index], progress));
                const double error = fitted - edge(actual, right);
                errors[right][index] += error * error;
            }
        }
    }
    const double minimumWidth = second ? kMinimumKshFieldWidth : fieldWidth;
    double bestError = errors[0][0] + errors[1][0];
    std::uint32_t flags = 0;
    for (std::size_t left = 0; left < kFieldEasings.size(); ++left) {
        for (std::size_t right = 0; right < kFieldEasings.size(); ++right) {
            const double error = errors[0][left] + errors[1][right];
            if (error < bestError && keepsMinimumWidth(from, to, minimumWidth, kFieldEasings[left], kFieldEasings[right])) {
                bestError = error;
                flags = fieldEasingFlag(kFieldEasings[left], false) | fieldEasingFlag(kFieldEasings[right], true);
            }
        }
    }

    return flags;
}

void appendField(const TimedChart& source, QVector<ChartNote>& notes, const LaserSegment& first,
    const LaserSegment* second, const double start, const double end, const double singleWidth,
    std::optional<ChartNote>& previous, std::uint64_t& group) {
    const double startMilliseconds = millisecondsAt(source, start);
    const double endMilliseconds = millisecondsAt(source, end);
    const qint64 startTime = std::max<qint64>(0, qRound64(startMilliseconds));
    const qint64 endTime = qRound64(endMilliseconds);

    if (endTime - startTime < kMinimumSustainedDuration) {
        return;
    }
    const double clippedStart = startMilliseconds < 0.0
        ? std::lerp(start, end, -startMilliseconds / (endMilliseconds - startMilliseconds)) : start;
    const Span from = laserSpan(first, second, clippedStart, singleWidth);
    const Span to = laserSpan(first, second, end, singleWidth);
    if (!previous || previous->endMilliseconds != startTime
        || std::abs(previous->endX - from.center) >= (previous->endWidth + from.width) * 0.5) {
        ++group;
    }
    const auto easing = fitFieldEdges(first, second, clippedStart, end, singleWidth, from, to);
    previous = ChartNote{.groupId = group, .auxiliary = easing, .startMilliseconds = startTime, .endMilliseconds = endTime,
        .startX = from.center, .endX = to.center, .startWidth = from.width, .endWidth = to.width,
        .kind = NoteKind::Sky, .side = FloorSide::Sky};
    notes.append(*previous);
}

void appendFlicks(QVector<ChartNote>& notes, const QVector<TimingPoint>& timing,
    const QVector<ChartNote>& preferred, const QVector<ChartNote>& other) {
    std::map<qint64, ChartNote> flicks;
    for (const auto* track : {&preferred, &other}) {
        for (ChartNote note : *track) {
            const qint64 time = snapImportTime(timing, note.startMilliseconds);
            note.startMilliseconds = time;
            note.endMilliseconds = time;
            flicks.try_emplace(time, note);
        }
    }
    for (const auto& entry : flicks) {
        notes.append(entry.second);
    }
}

void appendLaserNotes(const TimedChart& source, QVector<ChartNote>& notes, const QVector<TimingPoint>& timing,
    const double fieldWidth, const KshLaser flickPriority) {
    const LaserTrack blueTrack = readLaser(source, 0);
    const LaserTrack redTrack = readLaser(source, 1);
    const auto blue = restoreLaserCurves(blueTrack.segments);
    const auto red = restoreLaserCurves(redTrack.segments);
    QVector<double> boundaries;
    boundaries.reserve((blue.size() + red.size()) * 2 + source.timing.size());
    for (const auto* track : {&blue, &red}) {
        for (const LaserSegment& segment : *track) {
            boundaries.append(segment.startBeat);
            boundaries.append(segment.endBeat);
        }
    }
    for (const TimingMarker& marker : source.timing) {
        boundaries.append(marker.beat);
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    std::optional<ChartNote> previous;
    std::uint64_t group = 0;
    qsizetype blueIndex = 0;
    qsizetype redIndex = 0;
    for (qsizetype index = 1; index < boundaries.size(); ++index) {
        const double start = boundaries[index - 1];
        const double end = boundaries[index];
        const double middle = std::midpoint(start, end);
        const auto* blueSegment = segmentAt(blue, blueIndex, middle);
        const auto* redSegment = segmentAt(red, redIndex, middle);
        if (!blueSegment && !redSegment) {
            continue;
        }
        const LaserSegment& first = blueSegment ? *blueSegment : *redSegment;
        const LaserSegment* second = blueSegment ? redSegment : nullptr;
        QVector<double> pieces{start, end};
        const bool curved = first.curved() || (second && second->curved());
        const double halfWidth = (second ? kMinimumKshFieldWidth : fieldWidth) * 0.5;
        const auto centerAt = [&](const double beat) {
            const double firstX = first.positionAt(beat);

            return second ? std::midpoint(firstX, second->positionAt(beat)) : firstX;
        };
        if (second) {
            const auto differenceAt = [&](const double beat) { return first.positionAt(beat) - second->positionAt(beat); };
            addCrossings(pieces, start, end, differenceAt, -kMinimumKshFieldWidth, curved);
            addCrossings(pieces, start, end, differenceAt, kMinimumKshFieldWidth, curved);
        }
        addCrossings(pieces, start, end, centerAt, halfWidth, curved);
        addCrossings(pieces, start, end, centerAt, 1.0 - halfWidth, curved);
        std::sort(pieces.begin(), pieces.end());
        const qint64 endTime = qRound64(millisecondsAt(source, end));
        double pieceStart = start;
        qint64 pieceStartTime = std::max<qint64>(0, qRound64(millisecondsAt(source, start)));
        for (qsizetype piece = 1; piece + 1 < pieces.size(); ++piece) {
            const double split = pieces[piece];
            const qint64 splitTime = qRound64(millisecondsAt(source, split));
            if (splitTime - pieceStartTime < kMinimumSustainedDuration
                || endTime - splitTime < kMinimumSustainedDuration) {
                continue;
            }
            appendField(source, notes, first, second, pieceStart, split, fieldWidth, previous, group);
            pieceStart = split;
            pieceStartTime = splitTime;
        }
        appendField(source, notes, first, second, pieceStart, end, fieldWidth, previous, group);
    }
    if (flickPriority == KshLaser::Red) {
        appendFlicks(notes, timing, redTrack.flicks, blueTrack.flicks);
    } else {
        appendFlicks(notes, timing, blueTrack.flicks, redTrack.flicks);
    }
}

void resnapFieldTimes(QVector<ChartNote>& notes, const QVector<TimingPoint>& timing) {
    qint64 previousFieldEnd = 0;
    for (ChartNote& note : notes) {
        if (note.kind != NoteKind::Sky) {
            continue;
        }
        note.startMilliseconds = std::max(snapImportTime(timing, note.startMilliseconds), previousFieldEnd);
        note.endMilliseconds = snapImportTimeAfter(timing, note.endMilliseconds,
            note.startMilliseconds + kMinimumSustainedDuration);
        previousFieldEnd = note.endMilliseconds;
    }
}

[[nodiscard]] Difficulty readDifficulty(const Header& header) {
    const QString value = header.value(QStringLiteral("difficulty"), QStringLiteral("light"));
    if (value == u"light") {
        return Difficulty::Minimal;
    }
    if (value == u"challenge") {
        return Difficulty::Evolved;
    }
    if (value == u"extended") {
        return Difficulty::Ultimate;
    }

    return Difficulty::Forbidden;
}

[[nodiscard]] QString assetPath(const QDir& folder, const QString& name) {
    if (name.isEmpty()) {
        return {};
    }
    const QFileInfo file(folder, name.trimmed());

    return file.isFile() ? file.absoluteFilePath() : QString();
}

} // namespace

KshImportResult importKsh(const QString& filePath, const KshImportOptions& options) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {.error = file.errorString()};
    }
    const QByteArray bytes = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return {.error = file.errorString()};
    }
    const QStringList lines = decodeText(bytes).split(u'\n');
    Header header;
    qsizetype bodyStart = 0;
    for (; bodyStart < lines.size(); ++bodyStart) {
        const QString line = lines.at(bodyStart).trimmed();
        if (line == kMeasureSeparator) {
            break;
        }
        const qsizetype equals = line.indexOf(u'=');
        if (equals > 0 && !line.startsWith(u"//")) {
            header.insert(line.first(equals), line.sliced(equals + 1));
        }
    }
    if (bodyStart == lines.size()) {
        return {.error = QStringLiteral("The KSH chart has no measure separator.")};
    }
    const TimedChart timed = readTiming(lines, bodyStart + 1, header);
    if (timed.rows.isEmpty()) {
        return {.error = QStringLiteral("The KSH chart contains no note rows.")};
    }
    const QFileInfo fileInfo(filePath);
    const QDir folder = fileInfo.absoluteDir();
    const QString title = header.value(QStringLiteral("title"));
    const double previewStart = std::max(0, header.value(QStringLiteral("po")).toInt()) / 1000.0;
    const double previewLength = std::max(0, header.value(QStringLiteral("plength")).toInt()) / 1000.0;
    const double requestedWidth = std::isfinite(options.fieldWidthLanes)
        ? options.fieldWidthLanes : kDefaultKshFieldWidthLanes;
    const double fieldWidth = std::clamp(requestedWidth, kMinimumKshFieldWidthLanes, kMaximumKshFieldWidthLanes)
        / kMaximumKshFieldWidthLanes;

    KshImportResult result;
    result.metadata = {
        .artistName = header.value(QStringLiteral("artist")),
        .songName = title.isEmpty() ? fileInfo.completeBaseName() : title,
        .jacketDesigner = header.value(QStringLiteral("illustrator")),
        .previewStartSeconds = previewStart,
        .previewEndSeconds = previewStart + previewLength,
    };
    result.chart.difficulty = readDifficulty(header);
    result.chart.metadata = {
        .rating = std::clamp(header.value(QStringLiteral("level"), QStringLiteral("1")).toInt(), 1, 20),
        .charters = header.value(QStringLiteral("effect")),
    };
    result.chart.hitObjects.name = result.metadata.songName;
    result.chart.hitObjects.durationMilliseconds = std::max<qint64>(0, qRound64(timed.endMilliseconds));
    result.chart.timingPoints = editorTiming(timed);
    result.chart.speedEvents = {SpeedEvent{}};
    result.songPath = assetPath(folder, header.value(QStringLiteral("m")).section(u';', 0, 0));
    result.jacketPath = assetPath(folder, header.value(QStringLiteral("jacket")));

    auto& notes = result.chart.hitObjects.notes;
    appendGroundNotes(timed, notes, result.chart.timingPoints);
    appendLaserNotes(timed, notes, result.chart.timingPoints, fieldWidth, options.flickPriority);
    resnapFieldTimes(notes, result.chart.timingPoints);
    std::stable_sort(notes.begin(), notes.end(), [](const ChartNote& first, const ChartNote& second) {
        return std::tie(first.startMilliseconds, first.endMilliseconds)
            < std::tie(second.startMilliseconds, second.endMilliseconds);
    });
    std::uint64_t nextId = 1;
    for (ChartNote& note : notes) {
        note.id = nextId++;
        result.chart.hitObjects.durationMilliseconds =
            std::max(result.chart.hitObjects.durationMilliseconds, note.endMilliseconds);
    }

    return result;
}

} // namespace infalsus
