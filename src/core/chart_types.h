#pragma once

#include <QtCore/QString>
#include <QtCore/QVector>

#include <cstdint>

inline constexpr std::uint32_t kFlickRight = 1024;
inline constexpr std::uint32_t kFlickLeft = 4096;
inline constexpr double kMinimumFlickWidth = 0.06;
inline constexpr double kMinimumFieldWidth = 0.01;
inline constexpr qint64 kMinimumSustainedDurationMilliseconds = 2;

enum class NoteKind : std::uint8_t {
    Tap,
    Hold,
    Flick,
    Sky,
    Unknown
};

enum class FloorSide : std::uint8_t {
    Central,
    LeftOuter,
    RightOuter,
    Sky,
    Unknown
};

struct ChartNote {
    std::uint64_t id = 0;
    std::uint64_t groupId = 0;
    std::uint32_t auxiliary = 0;
    qint64 startMilliseconds = 0;
    qint64 endMilliseconds = 0;
    double startX = 0.0;
    double endX = 0.0;
    double startWidth = 0.0;
    double endWidth = 0.0;
    NoteKind kind = NoteKind::Unknown;
    FloorSide side = FloorSide::Unknown;
};

struct ChartData {
    QString name;
    QVector<ChartNote> notes;
    qint64 durationMilliseconds = 0;
};
