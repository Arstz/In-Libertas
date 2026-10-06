#include "core/chart_document.h"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QSaveFile>
#include <QtCore/QtEndian>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <limits>

namespace {

constexpr std::uint64_t kSeedConstantA = 0x3B314601E57A13ADULL;
constexpr std::uint64_t kSeedConstantB = 0x9E3779B185EBCA87ULL;
constexpr std::uint64_t kSeedConstantC = 0x2E4AB5CD2E6D12FDULL;
constexpr std::uint64_t kSeedConstantD = 0x94D049BB133111EBULL;
constexpr std::uint32_t kIcp1Magic = 0x31504349U;
constexpr std::uint16_t kFormatVersion = 1;
constexpr std::uint16_t kHeaderLength = 28;
constexpr std::uint16_t kNoteRecordLength = 80;
constexpr std::uint16_t kEventRecordLength = 32;
constexpr std::uint32_t kSideCentral = 1;
constexpr std::uint32_t kSideLeftOuter = 2;
constexpr std::uint32_t kSideRightOuter = 3;
constexpr std::uint32_t kSideSky = 4;
constexpr std::uint32_t kTypeTap = 1;
constexpr std::uint32_t kTypeHold = 2;
constexpr std::uint32_t kTypeFlick = 4;
constexpr std::uint32_t kTypeSky = 5;
constexpr double kSerializedOuterLaneWidth = 0.25;
constexpr double kSerializedRightOuterLaneX = 1.25;
constexpr std::size_t kProtectedFieldCount = 10;
constexpr std::size_t kDirectFieldCount = 8;
constexpr std::size_t kPairCount = 4;
constexpr std::size_t kNoteSectionOffset = kHeaderLength;
constexpr std::size_t kDirectFieldOffset = 48;
constexpr std::size_t kSideTypeField = 4;
constexpr std::size_t kAuxiliaryField = 5;
constexpr std::size_t kStartXPair = 0;
constexpr std::size_t kEndXPair = 1;
constexpr std::size_t kStartWidthPair = 2;
constexpr std::size_t kEndWidthPair = 3;

struct DecoderState {
    std::uint64_t a = 0;
    std::uint64_t b = 0;
    std::uint64_t c = 0;
    std::uint64_t d = 0;
    std::uint32_t counter = 0;
};

struct DecodedRecord {
    std::array<std::uint64_t, kProtectedFieldCount> fields{};
    std::array<std::uint32_t, kDirectFieldCount> direct{};
    std::array<std::int64_t, kPairCount> numerators{};
    std::array<std::int64_t, kPairCount> denominators{};
};

[[nodiscard]] std::uint64_t rotateLeft(const std::uint64_t value, const int shift) {
    return (value << shift) | (value >> (64 - shift));
}

[[nodiscard]] std::uint32_t readU32(const QByteArray& bytes, const std::size_t offset) {
    return qFromLittleEndian<std::uint32_t>(reinterpret_cast<const uchar*>(bytes.constData()) + offset);
}

[[nodiscard]] std::uint64_t readU64(const QByteArray& bytes, const std::size_t offset) {
    return qFromLittleEndian<std::uint64_t>(reinterpret_cast<const uchar*>(bytes.constData()) + offset);
}

void advanceState(DecoderState& state, const std::uint64_t first, const std::uint64_t second, const std::uint64_t third) {
    switch (state.counter & 3U) {
    case 0:
        state.a += first + rotateLeft(state.d, 7);
        state.b ^= rotateLeft(second + state.a, 13);
        state.c -= state.b ^ third;
        state.d = state.a ^ rotateLeft(state.c + state.d, 27);
        break;
    case 1:
        state.b += third + rotateLeft(state.a, 9);
        state.c ^= rotateLeft(first + state.b, 19);
        state.d -= state.c ^ second;
        state.a = state.b ^ rotateLeft(state.a + state.d, 31);
        break;
    case 2:
        state.c += second + rotateLeft(state.b, 15);
        state.d ^= rotateLeft(third + state.c, 23);
        state.a -= state.d ^ first;
        state.b = state.c ^ rotateLeft(state.a + state.b, 39);
        break;
    default: {
        const std::uint64_t oldC = state.c;
        state.d += first + rotateLeft(state.c, 21);
        state.a ^= rotateLeft(second + state.d, 29);
        state.b -= third ^ state.a;
        state.c = state.d ^ rotateLeft(state.b + oldC, 43);
        break;
    }
    }
    ++state.counter;
}

[[nodiscard]] DecoderState seedState(const QString& chartName, const std::uint32_t noteCount) {
    DecoderState state{
        .a = static_cast<std::uint64_t>(noteCount) - kSeedConstantA,
        .b = (static_cast<std::uint64_t>(noteCount) << 32) ^ kSeedConstantB,
        .c = 1ULL - kSeedConstantC,
        .d = 1ULL ^ kSeedConstantD,
        .counter = 0
    };

    for (int index = 0; index < chartName.size(); ++index) {
        const std::uint64_t character = (static_cast<std::uint64_t>(index) << 32)
            | chartName.at(index).unicode();
        const std::uint64_t modeMixed = character ^ 1ULL;
        const std::uint64_t countMixed = character + noteCount;
        advanceState(state, character, countMixed, modeMixed);
    }
    advanceState(state, noteCount, 1ULL, static_cast<std::uint64_t>(chartName.size()));

    return state;
}

[[nodiscard]] std::uint64_t maskForField(const DecoderState& state, const std::uint32_t noteIndex, const std::uint32_t fieldIndex) {
    const std::uint64_t packedIndex = (static_cast<std::uint64_t>(noteIndex) << 32) | fieldIndex;

    switch (fieldIndex & 3U) {
    case 0:
        return state.d ^ (state.a + rotateLeft(state.c ^ packedIndex, 11));
    case 1:
        return state.a ^ (state.b - rotateLeft(packedIndex + state.d, 17));
    case 2:
        return state.b ^ (state.c + rotateLeft(state.a ^ packedIndex, 29));
    default:
        return state.d ^ rotateLeft(packedIndex + state.b + state.c, 37);
    }
}

[[nodiscard]] std::int64_t positiveGcd(std::int64_t first, std::int64_t second) {
    first = std::llabs(first);
    second = std::llabs(second);
    if (first == 0 && second == 0) {
        return 1;
    }
    while (second != 0) {
        const std::int64_t remainder = first % second;
        first = second;
        second = remainder;
    }

    return first;
}

[[nodiscard]] std::uint32_t pairGcd(const std::uint32_t numerator, const std::uint32_t denominator) {
    return static_cast<std::uint32_t>(positiveGcd(static_cast<std::int32_t>(numerator), static_cast<std::int32_t>(denominator)));
}

[[nodiscard]] std::array<std::int64_t, 2> scalePair(
    const std::uint32_t scaleRaw,
    const std::uint32_t numeratorRaw,
    const std::uint32_t denominatorRaw) {
    const std::int64_t scale = static_cast<std::int32_t>(scaleRaw);
    const std::int64_t numerator = static_cast<std::int32_t>(numeratorRaw);
    const std::int64_t denominator = static_cast<std::int32_t>(denominatorRaw);
    const std::int64_t divisor = positiveGcd(numerator, denominator);

    return {scale * (numerator / divisor), scale * (denominator / divisor)};
}

[[nodiscard]] std::uint64_t pack(const std::uint32_t low, const std::uint32_t high) {
    return low | (static_cast<std::uint64_t>(high) << 32);
}

[[nodiscard]] std::uint64_t pack(const std::int64_t low, const std::int64_t high) {
    return pack(static_cast<std::uint32_t>(low), static_cast<std::uint32_t>(high));
}

void finalizeRecordState(
    DecoderState& state,
    const std::uint32_t noteIndex,
    const std::uint64_t fieldZero,
    const std::uint32_t firstPairGcd,
    const std::uint32_t secondPairGcd,
    const std::uint32_t thirdPairGcd,
    const std::uint32_t fourthPairGcd) {
    const std::uint64_t repeatedIndex = noteIndex | (static_cast<std::uint64_t>(noteIndex) << 32);
    const std::uint64_t foldedState = repeatedIndex
        + ((state.a + rotateLeft(state.b, 9)) ^ (state.d + rotateLeft(state.c, 23)));
    const std::uint32_t fold = static_cast<std::uint32_t>(foldedState ^ (foldedState >> 32));
    const std::uint64_t fourth = (static_cast<std::uint64_t>(fourthPairGcd) - 1ULL) ^ (fold >> 24);
    const std::uint64_t third = rotateLeft(
        (static_cast<std::uint64_t>(thirdPairGcd) - 1ULL) ^ ((fold >> 16) & 0xFFU), 29)
        ^ rotateLeft(fourth, 47);
    const std::uint64_t combined = (static_cast<std::uint64_t>(firstPairGcd) - 1ULL) ^ (fold & 0xFFU)
        ^ rotateLeft((static_cast<std::uint64_t>(secondPairGcd) - 1ULL) ^ ((fold >> 8) & 0xFFU), 13)
        ^ third;
    const std::uint32_t mix = static_cast<std::uint32_t>(fieldZero) ^ noteIndex
        ^ static_cast<std::uint32_t>(combined) ^ static_cast<std::uint32_t>(combined >> 32)
        ^ static_cast<std::uint32_t>(fieldZero >> 32);
    const std::uint64_t repeated = mix | (static_cast<std::uint64_t>(mix) << 32);

    state.a ^= repeated;
    state.b += rotateLeft(repeated, 17);
    state.c -= rotateLeft(repeated, 31);
    state.d ^= rotateLeft(repeated, 47);
}

[[nodiscard]] NoteKind noteKindForCode(const std::uint32_t code) {
    switch (code) {
    case kTypeTap:
        return NoteKind::Tap;
    case kTypeHold:
        return NoteKind::Hold;
    case kTypeFlick:
        return NoteKind::Flick;
    case kTypeSky:
        return NoteKind::Sky;
    default:
        return NoteKind::Unknown;
    }
}

[[nodiscard]] FloorSide floorSideForCode(const std::uint32_t code) {
    switch (code) {
    case kSideCentral:
        return FloorSide::Central;
    case kSideLeftOuter:
        return FloorSide::LeftOuter;
    case kSideRightOuter:
        return FloorSide::RightOuter;
    case kSideSky:
        return FloorSide::Sky;
    default:
        return FloorSide::Unknown;
    }
}

[[nodiscard]] double ratioValue(const std::int64_t numerator, const std::int64_t denominator) {
    if (denominator == 0) {
        return 0.0;
    }

    return static_cast<double>(numerator) / static_cast<double>(denominator);
}

[[nodiscard]] std::uint32_t sideCodeFor(const FloorSide side) {
    switch (side) {
    case FloorSide::Central:
        return kSideCentral;
    case FloorSide::LeftOuter:
        return kSideLeftOuter;
    case FloorSide::RightOuter:
        return kSideRightOuter;
    case FloorSide::Sky:
        return kSideSky;
    default:
        return 0;
    }
}

[[nodiscard]] std::uint32_t typeCodeFor(const NoteKind kind) {
    switch (kind) {
    case NoteKind::Tap:
        return kTypeTap;
    case NoteKind::Hold:
        return kTypeHold;
    case NoteKind::Flick:
        return kTypeFlick;
    case NoteKind::Sky:
        return kTypeSky;
    default:
        return 0;
    }
}

struct RawRatio {
    std::uint32_t scale = 1;
    std::uint32_t numerator = 0;
    std::uint32_t denominator = 1;
};

struct SerializedEvent {
    qint64 timeMilliseconds = 0;
    std::uint32_t kind = 0;
    std::uint64_t payloadA = 0;
    std::uint64_t payloadB = 0;
};

[[nodiscard]] RawRatio rawRatioFor(const double value, const std::int64_t denominator) {
    const std::int64_t safeDenominator = std::max<std::int64_t>(denominator, 1);
    const std::int64_t numerator = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(std::llround(value * safeDenominator)),
        std::numeric_limits<std::int32_t>::min(),
        std::numeric_limits<std::int32_t>::max());
    // The compiler uses the numerator/denominator grid to produce its ground
    // lane span. Reducing 2/4 to 1/2 is therefore not semantics-preserving:
    // it compiles as lane 1 instead of lane 2. The protected scale restores
    // this explicit grid after the decoder reduces the direct tail pair.
    const std::int64_t scale = positiveGcd(numerator, safeDenominator);

    return {
        .scale = static_cast<std::uint32_t>(scale),
        .numerator = static_cast<std::uint32_t>(numerator),
        .denominator = static_cast<std::uint32_t>(safeDenominator),
    };
}

void appendU32(QByteArray& bytes, const std::uint32_t value) {
    const int offset = bytes.size();
    bytes.resize(offset + static_cast<int>(sizeof(value)));
    qToLittleEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
}

void appendU64(QByteArray& bytes, const std::uint64_t value) {
    const int offset = bytes.size();
    bytes.resize(offset + static_cast<int>(sizeof(value)));
    qToLittleEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
}

void appendU16(QByteArray& bytes, const std::uint16_t value) {
    const int offset = bytes.size();
    bytes.resize(offset + static_cast<int>(sizeof(value)));
    qToLittleEndian(value, reinterpret_cast<uchar*>(bytes.data() + offset));
}

[[nodiscard]] bool zonesShareEndpoint(const ChartNote& first, const ChartNote& second) {
    constexpr qint64 kTimeToleranceMilliseconds = 1;
    constexpr double kCoordinateTolerance = 0.0001;
    const std::array<std::pair<qint64, std::pair<double, double>>, 2> firstEndpoints{{
        {first.startMilliseconds, {first.startX - first.startWidth * 0.5, first.startX + first.startWidth * 0.5}},
        {first.endMilliseconds, {first.endX - first.endWidth * 0.5, first.endX + first.endWidth * 0.5}},
    }};
    const std::array<std::pair<qint64, std::pair<double, double>>, 2> secondEndpoints{{
        {second.startMilliseconds, {second.startX - second.startWidth * 0.5, second.startX + second.startWidth * 0.5}},
        {second.endMilliseconds, {second.endX - second.endWidth * 0.5, second.endX + second.endWidth * 0.5}},
    }};
    for (const auto& firstEndpoint : firstEndpoints) {
        for (const auto& secondEndpoint : secondEndpoints) {
            if (std::abs(firstEndpoint.first - secondEndpoint.first) <= kTimeToleranceMilliseconds
                && std::abs(firstEndpoint.second.first - secondEndpoint.second.first) <= kCoordinateTolerance
                && std::abs(firstEndpoint.second.second - secondEndpoint.second.second) <= kCoordinateTolerance) {
                return true;
            }
        }
    }

    return false;
}

[[nodiscard]] QVector<std::uint64_t> canonicalGroupIds(const ChartData& chart) {
    QVector<std::uint64_t> groupIds(chart.notes.size());
    std::uint64_t nextGroupId = 0;
    for (int index = 0; index < chart.notes.size(); ++index) {
        const ChartNote& note = chart.notes.at(index);
        if (note.kind != NoteKind::Sky) {
            groupIds[index] = nextGroupId++;
            continue;
        }
        bool foundConnectedZone = false;
        for (int previousIndex = 0; previousIndex < index; ++previousIndex) {
            const ChartNote& previous = chart.notes.at(previousIndex);
            if (previous.kind == NoteKind::Sky && zonesShareEndpoint(previous, note)) {
                groupIds[index] = groupIds.at(previousIndex);
                foundConnectedZone = true;
                break;
            }
        }
        if (!foundConnectedZone) {
            groupIds[index] = nextGroupId++;
        }
    }

    return groupIds;
}

[[nodiscard]] bool hasValidCentralFloorSpan(const ChartNote& note) {
    constexpr double kFirstCentralLaneCoordinate = 0.25;
    constexpr double kLastCentralLaneCoordinate = 1.0;
    constexpr double kSingleLaneWidth = 0.25;
    constexpr double kRightCentralBoundary = 1.25;
    constexpr double kTolerance = 0.0001;
    const auto validEndpoint = [](const double coordinate, const double width) {
        return std::isfinite(coordinate) && std::isfinite(width)
            && coordinate >= kFirstCentralLaneCoordinate - kTolerance
            && coordinate <= kLastCentralLaneCoordinate + kTolerance
            && width >= kSingleLaneWidth - kTolerance
            && coordinate + width <= kRightCentralBoundary + kTolerance;
    };

    return validEndpoint(note.startX, note.startWidth)
        && validEndpoint(note.endX, note.endWidth);
}

[[nodiscard]] QString exportValidationError(const ChartData& chart) {
    for (const ChartNote& note : chart.notes) {
        if ((note.kind == NoteKind::Tap || note.kind == NoteKind::Hold)
            && note.side == FloorSide::Central && !hasValidCentralFloorSpan(note)) {
            return QStringLiteral("Hit object %1 has an invalid central lane span. "
                "Central floor objects must start at lanes 1 through 4 and remain within the central lanes.")
                .arg(note.id);
        }
    }

    return {};
}

} // namespace

bool ChartDocument::LoadResult::succeeded() const {
    return error.isEmpty();
}

ChartDocument::LoadResult ChartDocument::load(const QString& filePath) {
    const QFileInfo fileInfo(filePath);
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {.error = QStringLiteral("Could not open %1.").arg(fileInfo.fileName())};
    }

    const QByteArray bytes = file.readAll();
    if (bytes.size() < static_cast<qsizetype>(kHeaderLength)) {
        return {.error = QStringLiteral("The chart is shorter than an ICP1 header.")};
    }

    const std::uint32_t magic = readU32(bytes, 0);
    const std::uint16_t version = qFromLittleEndian<std::uint16_t>(reinterpret_cast<const uchar*>(bytes.constData()) + 4);
    const std::uint16_t headerLength = qFromLittleEndian<std::uint16_t>(reinterpret_cast<const uchar*>(bytes.constData()) + 6);
    const std::uint16_t noteLength = qFromLittleEndian<std::uint16_t>(reinterpret_cast<const uchar*>(bytes.constData()) + 8);
    const std::uint16_t eventLength = qFromLittleEndian<std::uint16_t>(reinterpret_cast<const uchar*>(bytes.constData()) + 10);
    if (magic != kIcp1Magic || version != kFormatVersion || headerLength != kHeaderLength
        || noteLength != kNoteRecordLength || eventLength != kEventRecordLength) {
        return {.error = QStringLiteral("The selected SPC is not an ICP1 v1 chart.")};
    }

    const std::uint32_t noteCount = readU32(bytes, 12);
    const std::uint32_t eventCount = readU32(bytes, 16);
    const std::uint64_t expectedLength = kHeaderLength
        + static_cast<std::uint64_t>(noteCount) * kNoteRecordLength
        + static_cast<std::uint64_t>(eventCount) * kEventRecordLength;
    if (expectedLength != static_cast<std::uint64_t>(bytes.size())) {
        return {.error = QStringLiteral("The SPC length does not match its record counts.")};
    }

    const QString chartName = fileInfo.fileName();
    DecoderState state = seedState(chartName, noteCount);
    ChartData chart;
    QVector<infalsus::TimingPoint> timingPoints;
    QVector<infalsus::LaneEvent> laneEvents;
    QVector<infalsus::SpeedEvent> speedEvents;
    chart.name = chartName;
    chart.notes.reserve(static_cast<qsizetype>(noteCount));
    const float headerBpm = std::bit_cast<float>(readU32(bytes, 20));
    const float headerMeter = std::bit_cast<float>(readU32(bytes, 24));
    timingPoints.append({
        .timeMilliseconds = 0,
        .beatsPerMinute = std::isfinite(headerBpm) && headerBpm > 0.0F ? headerBpm : 120.0,
        .timeSignatureNumerator = std::isfinite(headerMeter)
            ? std::clamp(static_cast<int>(std::lround(headerMeter)), 1, 32)
            : 4,
        .timeSignatureDenominator = 4,
    });

    for (std::uint32_t index = 0; index < noteCount; ++index) {
        const std::size_t recordOffset = kNoteSectionOffset + static_cast<std::size_t>(index) * kNoteRecordLength;
        DecodedRecord decoded;
        decoded.fields[0] = readU64(bytes, recordOffset) - maskForField(state, index, 0);
        decoded.fields[1] = readU64(bytes, recordOffset + 8) - maskForField(state, index, 1);
        for (std::uint32_t fieldIndex = 2; fieldIndex < kProtectedFieldCount; ++fieldIndex) {
            const std::size_t fieldOffset = recordOffset + fieldIndex * 4 + 8;
            decoded.fields[fieldIndex] = static_cast<std::uint32_t>(
                readU32(bytes, fieldOffset) - static_cast<std::uint32_t>(maskForField(state, index, fieldIndex)));
        }
        for (std::size_t directIndex = 0; directIndex < kDirectFieldCount; ++directIndex) {
            decoded.direct[directIndex] = readU32(bytes, recordOffset + kDirectFieldOffset + directIndex * 4);
        }
        for (std::size_t pairIndex = 0; pairIndex < kPairCount; ++pairIndex) {
            const auto pair = scalePair(
                static_cast<std::uint32_t>(decoded.fields[pairIndex + 6]),
                decoded.direct[pairIndex * 2],
                decoded.direct[pairIndex * 2 + 1]);
            decoded.numerators[pairIndex] = pair[0];
            decoded.denominators[pairIndex] = pair[1];
        }

        advanceState(state, decoded.fields[0], decoded.fields[1], pack(
            static_cast<std::uint32_t>(decoded.fields[2]), static_cast<std::uint32_t>(decoded.fields[3])));
        advanceState(state, decoded.fields[4], decoded.fields[5], pack(
            decoded.numerators[kStartXPair], decoded.denominators[kStartXPair]));
        advanceState(state,
            pack(decoded.numerators[kEndXPair], decoded.denominators[kEndXPair]),
            pack(decoded.numerators[kStartWidthPair], decoded.denominators[kStartWidthPair]),
            pack(decoded.numerators[kEndWidthPair], decoded.denominators[kEndWidthPair]));
        finalizeRecordState(state, index, decoded.fields[0],
            pairGcd(decoded.direct[0], decoded.direct[1]),
            pairGcd(decoded.direct[2], decoded.direct[3]),
            pairGcd(decoded.direct[4], decoded.direct[5]),
            pairGcd(decoded.direct[6], decoded.direct[7]));

        const std::uint32_t sideAndType = static_cast<std::uint32_t>(decoded.fields[kSideTypeField]);
        ChartNote note;
        note.id = decoded.fields[0];
        note.groupId = decoded.fields[1];
        note.auxiliary = static_cast<std::uint32_t>(decoded.fields[kAuxiliaryField]);
        note.startMilliseconds = static_cast<std::uint32_t>(decoded.fields[2]);
        note.endMilliseconds = static_cast<std::uint32_t>(decoded.fields[3]);
        note.startX = ratioValue(decoded.numerators[kStartXPair], decoded.denominators[kStartXPair]);
        note.endX = ratioValue(decoded.numerators[kEndXPair], decoded.denominators[kEndXPair]);
        note.startWidth = ratioValue(decoded.numerators[kStartWidthPair], decoded.denominators[kStartWidthPair]);
        note.endWidth = ratioValue(decoded.numerators[kEndWidthPair], decoded.denominators[kEndWidthPair]);
        note.kind = noteKindForCode((sideAndType >> 8) & 0xFFU);
        note.side = floorSideForCode(sideAndType & 0xFFU);
        chart.durationMilliseconds = std::max(chart.durationMilliseconds, note.endMilliseconds);
        chart.notes.append(note);
    }

    const std::size_t eventOffset = kNoteSectionOffset + static_cast<std::size_t>(noteCount) * kNoteRecordLength;
    for (std::uint32_t index = 0; index < eventCount; ++index) {
        const std::size_t recordOffset = eventOffset + static_cast<std::size_t>(index) * kEventRecordLength;
        const qint64 timeMilliseconds = readU32(bytes, recordOffset + 8);
        const std::uint32_t kind = readU32(bytes, recordOffset + 12);
        const std::uint64_t payloadA = readU64(bytes, recordOffset + 16);
        if (kind == 0) {
            const double speed = std::bit_cast<double>(payloadA);
            if (std::isfinite(speed)) {
                speedEvents.append({
                    .timeMilliseconds = timeMilliseconds,
                    .speed = speed,
                });
            }
        } else if (kind == 1) {
            const double bpm = std::bit_cast<double>(payloadA);
            const float meter = std::bit_cast<float>(readU32(bytes, recordOffset + 24));
            if (!std::isfinite(bpm) || bpm <= 0.0 || !std::isfinite(meter)) {
                continue;
            }
            const infalsus::TimingPoint timingPoint{
                .timeMilliseconds = timeMilliseconds,
                .beatsPerMinute = bpm,
                .timeSignatureNumerator = std::clamp(static_cast<int>(std::lround(meter)), 1, 32),
                // The current SPC representation stores only the meter value,
                // not a denominator, so preserve the editor's canonical 4.
                .timeSignatureDenominator = 4,
            };
            const auto existing = std::find_if(timingPoints.begin(), timingPoints.end(),
                [timeMilliseconds](const infalsus::TimingPoint& point) {
                    return point.timeMilliseconds == timeMilliseconds;
                });
            if (existing == timingPoints.end()) {
                timingPoints.append(timingPoint);
            } else {
                *existing = timingPoint;
            }
        } else if (kind == 3) {
            const int lane = static_cast<int>(payloadA & 0xFFU);
            if (lane > 5) {
                continue;
            }
            laneEvents.append({
                .timeMilliseconds = timeMilliseconds,
                .lane = lane,
                .enabled = (payloadA & 0x100U) != 0,
            });
        }
        chart.durationMilliseconds = std::max(chart.durationMilliseconds, timeMilliseconds);
    }
    std::sort(timingPoints.begin(), timingPoints.end(), [](const infalsus::TimingPoint& first,
        const infalsus::TimingPoint& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    std::stable_sort(laneEvents.begin(), laneEvents.end(), [](const infalsus::LaneEvent& first,
        const infalsus::LaneEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    std::stable_sort(speedEvents.begin(), speedEvents.end(), [](const infalsus::SpeedEvent& first,
        const infalsus::SpeedEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });

    return {
        .chart = std::move(chart),
        .timingPoints = std::move(timingPoints),
        .laneEvents = std::move(laneEvents),
        .speedEvents = std::move(speedEvents),
    };
}

bool saveEncodedChart(const QString& filePath, const QString& chartName, const ChartData& chart,
    const QVector<infalsus::TimingPoint>& sourceTimingPoints, const QVector<infalsus::LaneEvent>& sourceLaneEvents,
    const QVector<infalsus::SpeedEvent>& sourceSpeedEvents,
    QString* error) {
    if (chart.notes.size() > std::numeric_limits<std::uint32_t>::max()) {
        *error = QStringLiteral("The chart has too many hit objects for SPC.");
        return false;
    }
    if (const QString validationError = exportValidationError(chart); !validationError.isEmpty()) {
        *error = validationError;
        return false;
    }
    QVector<infalsus::TimingPoint> timingPoints = sourceTimingPoints;
    if (timingPoints.isEmpty()) {
        timingPoints.append(infalsus::TimingPoint{});
    }
    std::sort(timingPoints.begin(), timingPoints.end(), [](const infalsus::TimingPoint& first, const infalsus::TimingPoint& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    // ICP1 playback compilation requires an initial timing state. The editor
    // permits a first visible timing point after an audio lead-in, but the
    // native compiler still indexes timing data from song time zero. Preserve
    // the authored point and synthesize the identical base state for export.
    if (timingPoints.front().timeMilliseconds > 0) {
        infalsus::TimingPoint basePoint = timingPoints.front();
        basePoint.timeMilliseconds = 0;
        timingPoints.prepend(basePoint);
    }
    QVector<SerializedEvent> events;
    events.reserve(sourceLaneEvents.size() + sourceSpeedEvents.size() + timingPoints.size());
    for (const infalsus::SpeedEvent& speedEvent : sourceSpeedEvents) {
        if (!std::isfinite(speedEvent.speed)) {
            continue;
        }
        events.append({
            .timeMilliseconds = std::max<qint64>(0, speedEvent.timeMilliseconds),
            .kind = 0,
            .payloadA = std::bit_cast<std::uint64_t>(speedEvent.speed),
            .payloadB = 0,
        });
    }
    for (const infalsus::LaneEvent& laneEvent : sourceLaneEvents) {
        const std::uint64_t lane = static_cast<std::uint64_t>(std::clamp(laneEvent.lane, 0, 5));
        events.append({
            .timeMilliseconds = std::max<qint64>(0, laneEvent.timeMilliseconds),
            .kind = 3,
            .payloadA = lane | (laneEvent.enabled ? 0x100U : 0U),
            .payloadB = 0,
        });
    }
    // Emit every authored BPM section. The header supplies an initial state,
    // but it is not a replacement for a type-1 event: vanilla charts retain
    // explicit sections even when their values match the preceding section.
    for (int index = 0; index < timingPoints.size(); ++index) {
        const infalsus::TimingPoint& point = timingPoints.at(index);
        const double bpm = point.beatsPerMinute;
        const float meter = static_cast<float>(point.timeSignatureNumerator);
        events.append({
            .timeMilliseconds = point.timeMilliseconds,
            .kind = 1,
            .payloadA = std::bit_cast<std::uint64_t>(bpm),
            .payloadB = std::bit_cast<std::uint32_t>(meter),
        });
    }
    std::stable_sort(events.begin(), events.end(), [](const SerializedEvent& first, const SerializedEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    const std::uint32_t noteCount = static_cast<std::uint32_t>(chart.notes.size());
    const std::uint32_t eventCount = static_cast<std::uint32_t>(events.size());
    const infalsus::TimingPoint& initialPoint = timingPoints.front();
    // The runtime's playback-buffer compiler consumes source records in
    // chronological order.  The editor deliberately keeps creation order for
    // stable selection/history, so derive an ordered export view rather than
    // reordering the document itself.
    ChartData orderedChart = chart;
    std::stable_sort(orderedChart.notes.begin(), orderedChart.notes.end(),
        [](const ChartNote& first, const ChartNote& second) {
            if (first.startMilliseconds != second.startMilliseconds) {
                return first.startMilliseconds < second.startMilliseconds;
            }
            return first.endMilliseconds < second.endMilliseconds;
        });
    const QVector<std::uint64_t> groupIds = canonicalGroupIds(orderedChart);
    QByteArray bytes;
    bytes.reserve(kHeaderLength + static_cast<int>(noteCount) * kNoteRecordLength
        + static_cast<int>(eventCount) * kEventRecordLength);
    appendU32(bytes, kIcp1Magic);
    appendU16(bytes, kFormatVersion);
    appendU16(bytes, kHeaderLength);
    appendU16(bytes, kNoteRecordLength);
    appendU16(bytes, kEventRecordLength);
    appendU32(bytes, noteCount);
    appendU32(bytes, eventCount);
    appendU32(bytes, std::bit_cast<std::uint32_t>(static_cast<float>(initialPoint.beatsPerMinute)));
    appendU32(bytes, std::bit_cast<std::uint32_t>(static_cast<float>(initialPoint.timeSignatureNumerator)));

    DecoderState state = seedState(chartName, noteCount);
    for (std::uint32_t index = 0; index < noteCount; ++index) {
        const ChartNote& note = orderedChart.notes.at(static_cast<int>(index));
        // The editor represents either slanted outer lane as a whole visual
        // branch at X=0 with width one. ICP1 instead uses the shared
        // quarter-grid coordinate system: an outer-lane Tap/Hold must occupy
        // width 1/4, and the right branch must use X=5/4. Both are inputs to
        // the game's floor-buffer index calculation.
        const bool isOuterFloorLane = note.side == FloorSide::LeftOuter
            || note.side == FloorSide::RightOuter;
        const double startX = note.side == FloorSide::RightOuter
            ? kSerializedRightOuterLaneX
            : note.startX;
        const double endX = note.side == FloorSide::RightOuter
            ? kSerializedRightOuterLaneX
            : note.endX;
        const double startWidth = isOuterFloorLane ? kSerializedOuterLaneWidth : note.startWidth;
        const double endWidth = isOuterFloorLane ? kSerializedOuterLaneWidth : note.endWidth;
        // Floor co-ordinates use an explicit quarter grid in the game. Sky
        // objects retain the editor's high-resolution co-ordinates so zones
        // and flicks do not drift when they are saved and re-opened.
        constexpr std::int64_t kFloorGridDenominator = 4;
        constexpr std::int64_t kSkyCoordinateDenominator = 1000000;
        const std::int64_t coordinateDenominator = note.side == FloorSide::Sky
            ? kSkyCoordinateDenominator
            : kFloorGridDenominator;
        const std::array<RawRatio, 4> ratios{{
            rawRatioFor(startX, coordinateDenominator), rawRatioFor(endX, coordinateDenominator),
            rawRatioFor(startWidth, coordinateDenominator), rawRatioFor(endWidth, coordinateDenominator),
        }};
        const std::array<std::uint64_t, kProtectedFieldCount> fields{{
            index,
            groupIds.at(static_cast<int>(index)),
            static_cast<std::uint32_t>(std::max<qint64>(0, note.startMilliseconds)),
            static_cast<std::uint32_t>(std::max<qint64>(0, note.endMilliseconds)),
            sideCodeFor(note.side) | (typeCodeFor(note.kind) << 8U),
            note.auxiliary,
            ratios[0].scale,
            ratios[1].scale,
            ratios[2].scale,
            ratios[3].scale,
        }};
        appendU64(bytes, fields[0] + maskForField(state, index, 0));
        appendU64(bytes, fields[1] + maskForField(state, index, 1));
        for (std::uint32_t field = 2; field < kProtectedFieldCount; ++field) {
            appendU32(bytes, static_cast<std::uint32_t>(fields[field])
                + static_cast<std::uint32_t>(maskForField(state, index, field)));
        }
        for (const RawRatio& ratio : ratios) {
            appendU32(bytes, ratio.numerator);
            appendU32(bytes, ratio.denominator);
        }
        const std::array<std::int64_t, kPairCount> numerators{{
            static_cast<std::int32_t>(ratios[0].numerator), static_cast<std::int32_t>(ratios[1].numerator),
            static_cast<std::int32_t>(ratios[2].numerator), static_cast<std::int32_t>(ratios[3].numerator),
        }};
        const std::array<std::int64_t, kPairCount> denominators{{
            static_cast<std::int32_t>(ratios[0].denominator), static_cast<std::int32_t>(ratios[1].denominator),
            static_cast<std::int32_t>(ratios[2].denominator), static_cast<std::int32_t>(ratios[3].denominator),
        }};
        advanceState(state, fields[0], fields[1], pack(static_cast<std::uint32_t>(fields[2]), static_cast<std::uint32_t>(fields[3])));
        advanceState(state, fields[4], fields[5], pack(numerators[0], denominators[0]));
        advanceState(state, pack(numerators[1], denominators[1]), pack(numerators[2], denominators[2]),
            pack(numerators[3], denominators[3]));
        finalizeRecordState(state, index, fields[0],
            pairGcd(ratios[0].numerator, ratios[0].denominator),
            pairGcd(ratios[1].numerator, ratios[1].denominator),
            pairGcd(ratios[2].numerator, ratios[2].denominator),
            pairGcd(ratios[3].numerator, ratios[3].denominator));
    }
    for (std::uint32_t index = 0; index < eventCount; ++index) {
        const SerializedEvent& event = events.at(static_cast<int>(index));
        appendU64(bytes, index);
        appendU32(bytes, static_cast<std::uint32_t>(std::max<qint64>(0, event.timeMilliseconds)));
        appendU32(bytes, event.kind);
        appendU64(bytes, event.payloadA);
        appendU64(bytes, event.payloadB);
    }
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        *error = QStringLiteral("Could not write the SPC file.");
        return false;
    }

    return true;
}

bool ChartDocument::save(const QString& filePath, const ChartData& chart,
    const QVector<infalsus::TimingPoint>& timingPoints, const QVector<infalsus::LaneEvent>& laneEvents,
    const QVector<infalsus::SpeedEvent>& speedEvents, QString* error) {
    return saveEncodedChart(filePath, QFileInfo(filePath).fileName(), chart, timingPoints, laneEvents, speedEvents, error);
}
