#include "core/field_presets.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace infalsus {

namespace {

double circleWidth(const double progress) {
    return 2.0 * std::sqrt(std::max(0.0, progress * (1.0 - progress)));
}

double circleWidthAt(const qint64 time, const qint64 duration) {
    const qint64 midpoint = duration / 2 + duration % 2;
    const double progress = time <= midpoint ? 0.5 * time / midpoint
        : 0.5 + 0.5 * (time - midpoint) / (duration - midpoint);
    return circleWidth(progress);
}

}

FieldPreset circleFieldPreset() {
    FieldPreset preset{.name = QStringLiteral("Circle"), .circle = true};
    constexpr int samples = 128;
    for (int index = 0; index < samples; ++index) {
        const double start = index == samples / 2 ? 0.5
            : (1.0 - std::cos(std::numbers::pi * index / samples)) * 0.5;
        const double end = index + 1 == samples / 2 ? 0.5
            : (1.0 - std::cos(std::numbers::pi * (index + 1) / samples)) * 0.5;
        preset.segments.append({.start = start, .end = end,
            .startWidth = circleWidth(start), .endWidth = circleWidth(end)});
    }
    return preset;
}

FieldPreset captureFieldPreset(const QVector<ChartNote>& fields, const QString& name) {
    FieldPreset preset{.name = name};
    if (fields.isEmpty()) {
        return preset;
    }
    qint64 start = std::numeric_limits<qint64>::max();
    qint64 end = 0;
    for (const ChartNote& field : fields) {
        if (field.kind != NoteKind::Sky || field.endMilliseconds <= field.startMilliseconds) {
            return preset;
        }
        start = std::min(start, field.startMilliseconds);
        end = std::max(end, field.endMilliseconds);
    }
    const double duration = static_cast<double>(end - start);
    for (const ChartNote& field : fields) {
        preset.segments.append({
            .start = (field.startMilliseconds - start) / duration,
            .end = (field.endMilliseconds - start) / duration,
            .startX = field.startX,
            .endX = field.endX,
            .startWidth = field.startWidth,
            .endWidth = field.endWidth,
            .auxiliary = field.auxiliary,
        });
    }
    return preset;
}

QVector<ChartNote> instantiateFieldPreset(const FieldPreset& preset, const qint64 startMilliseconds,
    const qint64 durationMilliseconds) {
    QVector<ChartNote> fields;
    if (startMilliseconds < 0 || durationMilliseconds < (preset.circle ? 4 : 2)
        || startMilliseconds > std::numeric_limits<qint64>::max() - durationMilliseconds) {
        return fields;
    }
    fields.reserve(preset.segments.size());
    const qint64 midpoint = durationMilliseconds / 2 + durationMilliseconds % 2;
    qint64 previousEnd = 0;
    for (const FieldPresetSegment& segment : preset.segments) {
        const qint64 start = preset.circle ? previousEnd : std::llround(segment.start * durationMilliseconds);
        const qint64 end = std::llround(segment.end * durationMilliseconds);
        if (preset.circle) {
            if (end - start < 2 || (end < durationMilliseconds && durationMilliseconds - end < 2)
                || (end < midpoint && midpoint - end < 2)) {
                continue;
            }
        } else if (end - start < 2) {
            return {};
        }
        const double startWidth = preset.circle
            ? circleWidthAt(start, durationMilliseconds) : segment.startWidth;
        const double endWidth = preset.circle
            ? circleWidthAt(end, durationMilliseconds) : segment.endWidth;
        fields.append({.auxiliary = segment.auxiliary,
            .startMilliseconds = startMilliseconds + start, .endMilliseconds = startMilliseconds + end,
            .startX = segment.startX,
            .endX = segment.endX,
            .startWidth = startWidth, .endWidth = endWidth,
            .kind = NoteKind::Sky, .side = FloorSide::Sky});
        previousEnd = end;
    }
    return fields;
}

}
