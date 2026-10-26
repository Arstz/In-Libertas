#pragma once

#include "core/chart_types.h"

namespace infalsus {

struct FieldPresetSegment {
    double start = 0.0;
    double end = 1.0;
    double startX = 0.5;
    double endX = 0.5;
    double startWidth = 0.0;
    double endWidth = 0.0;
    std::uint32_t auxiliary = 0x24U;
};

struct FieldPreset {
    QString name;
    QVector<FieldPresetSegment> segments;
    bool circle = false;
};

[[nodiscard]] FieldPreset circleFieldPreset();
[[nodiscard]] FieldPreset captureFieldPreset(const QVector<ChartNote>& fields, const QString& name);
[[nodiscard]] QVector<ChartNote> instantiateFieldPreset(const FieldPreset& preset, qint64 startMilliseconds,
    qint64 durationMilliseconds);

}
