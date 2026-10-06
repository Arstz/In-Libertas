#include "gui/canvas/flat_view.h"

#include "gui/app/game_palette.h"

#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QCursor>
#include <QtGui/QMouseEvent>
#include <QtGui/QWheelEvent>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QLineF>
#include <QtCore/QProcess>
#include <QtCore/QSet>
#include <QtMultimedia/QAudioBuffer>
#include <QtMultimedia/QAudioDecoder>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace infalsus::gui {

namespace {

constexpr double kMinimumPixelsPerSecond = 48.0;
constexpr double kMaximumPixelsPerSecond = 720.0;
constexpr double kZoomStepFactor = 1.35;
constexpr double kWaveformWidth = 184.0;
constexpr double kWaveformWidthFraction = 0.45;
constexpr int kWaveformPointsPerSecond = static_cast<int>(kMaximumPixelsPerSecond);
constexpr double kMargin = 12.0;
constexpr qint64 kTapDurationMilliseconds = 30;
constexpr qint64 kMinimumHoldDurationMilliseconds = 80;
constexpr double kHoldEndEdgeHitDistance = 8.0;
constexpr double kDefaultZoneWidth = 0.18;
constexpr double kMinimumFlickWidth = 0.06;
constexpr double kMouseDragDistance = 4.0;
constexpr int kZoneSamples = 18;
constexpr double kZoneControlRadius = 5.0;
constexpr double kZoneEdgeHitDistance = 10.0;
constexpr double kMinimumZoneWidth = 0.01;
constexpr double kZoneSnapCoordinateDistance = 0.035;
constexpr qint64 kZoneSnapTimeDistanceMilliseconds = 32;
constexpr std::uint32_t kZoneLinear = 0x24U;
constexpr int kFlickTrailSamples = 18;
constexpr double kFlickTriangleHeight = 45.0;
constexpr double kMinimumFlickTrailWidth = 0.06;
constexpr std::uint32_t kCenterSineOut = 0x08U;
constexpr std::uint32_t kCenterSineIn = 0x10U;
constexpr std::uint32_t kWidthSineOut = 0x40U;
constexpr std::uint32_t kWidthSineIn = 0x80U;
constexpr std::uint32_t kFlickRight = 0x400U;
constexpr std::uint32_t kFlickLeft = 0x1000U;
constexpr int kInactiveOpacity = 50;

struct LaneSpan {
    double left = 0.0;
    double right = 1.0;
};

struct SkySpan {
    double left = 0.0;
    double right = 1.0;
};

[[nodiscard]] bool isMultiLane(const ChartNote& note) {
    constexpr double kSingleLaneWidth = 0.25;

    return note.side == FloorSide::Central
        && std::max(note.startWidth, note.endWidth) > kSingleLaneWidth;
}

[[nodiscard]] QColor groundColor(const ChartNote& note, const int opacity) {
    if (note.side == FloorSide::LeftOuter) {
        return palette::withAlpha(palette::outerNotePurple, opacity);
    }
    if (note.side == FloorSide::RightOuter) {
        return palette::withAlpha(palette::outerNoteRed, opacity);
    }
    if (note.kind == NoteKind::Hold) {
        return palette::withAlpha(palette::floorNoteBlue, opacity);
    }
    if (!isMultiLane(note)) {
        return palette::withAlpha(palette::floorNoteBlue, opacity);
    }

    return palette::withAlpha(palette::white, opacity);
}

[[nodiscard]] LaneSpan laneSpan(const ChartNote& note, const double firstLaneCenter, const double width) {
    switch (note.side) {
    case FloorSide::LeftOuter:
        return {.left = 0.0, .right = 1.0};
    case FloorSide::RightOuter:
        return {.left = 5.0, .right = 6.0};
    case FloorSide::Central: {
        const int firstLane = std::clamp(static_cast<int>(std::lround(firstLaneCenter * 4.0)), 1, 4);
        const int laneCount = std::clamp(static_cast<int>(std::lround(width * 4.0)), 1, 5 - firstLane);

        return {.left = static_cast<double>(firstLane), .right = static_cast<double>(firstLane + laneCount)};
    }
    default:
        return {};
    }
}

[[nodiscard]] FloorSide floorSideForLane(const int lane) {
    if (lane == 0) {
        return FloorSide::LeftOuter;
    }
    if (lane == 5) {
        return FloorSide::RightOuter;
    }

    return FloorSide::Central;
}

[[nodiscard]] double easingFor(const std::uint32_t auxiliary, const bool rightSide, const double progress) {
    const std::uint32_t mode = rightSide ? (auxiliary & 0xE0U) : (auxiliary & 0x1CU);
    const double clampedProgress = std::clamp(progress, 0.0, 1.0);
    const std::uint32_t sineOut = rightSide ? kWidthSineOut : kCenterSineOut;
    const std::uint32_t sineIn = rightSide ? kWidthSineIn : kCenterSineIn;

    if (mode == sineOut) {
        return std::sin(clampedProgress * std::numbers::pi * 0.5);
    }
    if (mode == sineIn) {
        return 1.0 - std::cos(clampedProgress * std::numbers::pi * 0.5);
    }

    return clampedProgress;
}

[[nodiscard]] double zoneLeftEdge(const ChartNote& hitObject, const bool atStart) {
    const double center = atStart ? hitObject.startX : hitObject.endX;
    const double width = atStart ? hitObject.startWidth : hitObject.endWidth;

    return center - width * 0.5;
}

[[nodiscard]] double zoneRightEdge(const ChartNote& hitObject, const bool atStart) {
    const double center = atStart ? hitObject.startX : hitObject.endX;
    const double width = atStart ? hitObject.startWidth : hitObject.endWidth;

    return center + width * 0.5;
}

void setZoneEndpoint(ChartNote& hitObject, const bool atStart, const double left, const double right) {
    const double clampedLeft = std::clamp(left, 0.0, 1.0 - kMinimumZoneWidth);
    const double clampedRight = std::clamp(right, clampedLeft + kMinimumZoneWidth, 1.0);
    const double center = (clampedLeft + clampedRight) * 0.5;
    const double width = clampedRight - clampedLeft;
    if (atStart) {
        hitObject.startX = center;
        hitObject.startWidth = width;
    } else {
        hitObject.endX = center;
        hitObject.endWidth = width;
    }
}

[[nodiscard]] SkySpan flickSpan(const ChartNote& hitObject) {
    const double width = std::clamp(
        hitObject.startWidth > 0.0 ? hitObject.startWidth : hitObject.endWidth,
        kMinimumFlickTrailWidth,
        1.0);

    return {
        .left = hitObject.startX - width * 0.5,
        .right = hitObject.startX + width * 0.5,
    };
}

void setFlickSpan(ChartNote& hitObject, const double left, const double right) {
    const double clampedLeft = std::clamp(left, 0.0, 1.0 - kMinimumFlickWidth);
    const double clampedRight = std::clamp(right, clampedLeft + kMinimumFlickWidth, 1.0);
    const double center = (clampedLeft + clampedRight) * 0.5;
    const double width = clampedRight - clampedLeft;
    hitObject.startX = center;
    hitObject.endX = center;
    hitObject.startWidth = width;
    hitObject.endWidth = width;
}

} // namespace

FlatView::FlatView(QWidget* parent)
    : QOpenGLWidget(parent) {
    setMinimumSize(240, 300);
    setFocusPolicy(Qt::StrongFocus);
    m_waveformProcess = new QProcess(this);
    connect(m_waveformProcess, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](const int exitCode, const QProcess::ExitStatus exitStatus) {
            if (exitCode == 0 && exitStatus == QProcess::NormalExit) {
                loadWaveform(m_waveformProcess->readAllStandardOutput());
            }
        });
    m_waveformDecoder = new QAudioDecoder(this);
    connect(m_waveformDecoder, &QAudioDecoder::bufferReady, this, [this] {
        while (m_waveformDecoder->bufferAvailable()) {
            appendWaveformAudio(m_waveformDecoder->read());
        }
    });
    connect(m_waveformDecoder, &QAudioDecoder::finished, this, [this] {
        while (m_waveformDecoder->bufferAvailable()) {
            appendWaveformAudio(m_waveformDecoder->read());
        }
        if (m_waveformFramesInPeak > 0) {
            m_waveformPeaks.append(QPointF(m_waveformMinimum, m_waveformMaximum));
            m_waveformFramesInPeak = 0;
        }
        update();
    });
}

void FlatView::setChart(const ChartData& chart) {
    m_chart = chart;
    invalidateSelectedZoneConnections();
    update();
}

void FlatView::setPlaybackPosition(const qint64 positionMilliseconds) {
    if (m_playbackPositionMilliseconds == positionMilliseconds) {
        return;
    }

    m_playbackPositionMilliseconds = positionMilliseconds;
    update();
}

void FlatView::setMode(const FlatViewMode mode) {
    m_mode = mode;
    update();
}

void FlatView::setTool(const EditorTool tool) {
    if (m_tool == tool) {
        return;
    }

    m_tool = tool;
}

void FlatView::setGridDuration(const qint64 durationMilliseconds) {
    m_gridDurationMilliseconds = std::max<qint64>(1, durationMilliseconds);
}

void FlatView::setDivisor(const int divisor) {
    if (m_divisor == divisor) {
        return;
    }

    m_divisor = divisor;
    update();
}

void FlatView::setTimingPoints(QVector<TimingPoint> timingPoints) {
    std::sort(timingPoints.begin(), timingPoints.end(), [](const TimingPoint& first, const TimingPoint& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_timingPoints = std::move(timingPoints);
    update();
}

void FlatView::setLaneEvents(QVector<LaneEvent> laneEvents) {
    std::stable_sort(laneEvents.begin(), laneEvents.end(), [](const LaneEvent& first, const LaneEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_laneEvents = std::move(laneEvents);
    update();
}

void FlatView::setSelectedHitObjects(QVector<int> indexes) {
    indexes.erase(std::remove_if(indexes.begin(), indexes.end(), [this](const int index) {
        return index < 0 || index >= m_chart.notes.size();
    }), indexes.end());
    std::sort(indexes.begin(), indexes.end());
    indexes.erase(std::unique(indexes.begin(), indexes.end()), indexes.end());
    if (m_selectedHitObjects == indexes) {
        return;
    }

    m_selectedHitObjects = std::move(indexes);
    m_selectedHitObjectIndexes.clear();
    m_selectedHitObjectIndexes.reserve(m_selectedHitObjects.size());
    for (const int index : m_selectedHitObjects) {
        m_selectedHitObjectIndexes.insert(index);
    }
    invalidateSelectedZoneConnections();
    update();
}

void FlatView::setAudioSource(const QString& audioPath) {
    if (m_waveformProcess->state() != QProcess::NotRunning) {
        m_waveformProcess->kill();
    }
    if (m_waveformDecoder->isDecoding()) {
        m_waveformDecoder->stop();
    }
    m_waveformBuffer.close();
    m_waveformBuffer.setBuffer(nullptr);
    m_waveformAudioData.clear();
    m_waveformPeaks.clear();
    m_waveformMillisecondsPerPeak = 10.0;
    if (audioPath.isEmpty()) {
        update();
        return;
    }

    const QString toolPath = waveformToolPath();
    if (!QFileInfo(toolPath).isExecutable()) {
        update();
        return;
    }
    m_waveformProcess->start(toolPath, {
        QStringLiteral("--input-filename"), audioPath,
        QStringLiteral("--output-filename"), QStringLiteral("-"),
        QStringLiteral("--output-format"), QStringLiteral("json"),
        QStringLiteral("--pixels-per-second"), QString::number(kWaveformPointsPerSecond),
        QStringLiteral("--bits"), QStringLiteral("8"),
        QStringLiteral("--quiet"),
    });
    update();
}

void FlatView::setAudioData(QByteArray audioData, const QString& fileName) {
    Q_UNUSED(fileName)

    if (m_waveformProcess->state() != QProcess::NotRunning) {
        m_waveformProcess->kill();
    }
    if (m_waveformDecoder->isDecoding()) {
        m_waveformDecoder->stop();
    }
    m_waveformPeaks.clear();
    m_waveformMillisecondsPerPeak = 1000.0 / kWaveformPointsPerSecond;
    m_waveformFramesPerPeak = 1;
    m_waveformFramesInPeak = 0;
    m_waveformMinimum = 0.0;
    m_waveformMaximum = 0.0;
    m_waveformBuffer.close();
    m_waveformAudioData = std::move(audioData);
    if (m_waveformAudioData.isEmpty()) {
        update();
        return;
    }

    m_waveformBuffer.setBuffer(&m_waveformAudioData);
    m_waveformBuffer.open(QIODevice::ReadOnly);
    m_waveformDecoder->setSourceDevice(&m_waveformBuffer);
    m_waveformDecoder->start();
    update();
}

void FlatView::addHitObject(const int index, const ChartNote& hitObject) {
    const int insertionIndex = std::clamp(index, 0, static_cast<int>(m_chart.notes.size()));
    m_chart.notes.insert(insertionIndex, hitObject);
    invalidateSelectedZoneConnections();
    update();
}

void FlatView::removeHitObject(const int index) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes.removeAt(index);
    invalidateSelectedZoneConnections();
    update();
}

void FlatView::updateHitObject(const int index, const ChartNote& hitObject) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes[index] = hitObject;
    invalidateSelectedZoneConnections();
    update();
}

void FlatView::updateHitObjects(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects) {
    if (indexes.size() != hitObjects.size()) {
        return;
    }

    bool changed = false;
    for (int itemIndex = 0; itemIndex < indexes.size(); ++itemIndex) {
        const int index = indexes.at(itemIndex);
        if (index < 0 || index >= m_chart.notes.size()) {
            continue;
        }
        m_chart.notes[index] = hitObjects.at(itemIndex);
        changed = true;
    }
    if (changed) {
        invalidateSelectedZoneConnections();
        update();
    }
}

void FlatView::zoom(const double steps) {
    if (std::abs(steps) < 0.001) {
        return;
    }

    m_pixelsPerSecond = std::clamp(
        m_pixelsPerSecond * std::pow(kZoomStepFactor, steps),
        kMinimumPixelsPerSecond,
        kMaximumPixelsPerSecond);
    update();
}

int FlatView::playheadViewportY() const {
    return static_cast<int>(std::lround(playheadY()));
}

QRectF FlatView::groundArea() const {
    const QRectF content = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
    const double waveformWidth = std::min(kWaveformWidth, content.width() * kWaveformWidthFraction);

    return QRectF(
        content.left() + waveformWidth + 6.0,
        content.top(),
        content.width() - waveformWidth - 6.0,
        content.height());
}

qint64 FlatView::timeAtY(const double y) const {
    const double offsetMilliseconds = (playheadY() - y) * 1000.0 / m_pixelsPerSecond;

    return m_playbackPositionMilliseconds + static_cast<qint64>(std::llround(offsetMilliseconds));
}

qint64 FlatView::snappedTimeAtY(const double y) const {
    const qint64 time = std::max<qint64>(0, timeAtY(y));
    if (m_timingPoints.isEmpty()) {
        return static_cast<qint64>(std::llround(
            static_cast<double>(time) / m_gridDurationMilliseconds)) * m_gridDurationMilliseconds;
    }

    const TimingPoint* timingPoint = &m_timingPoints.front();
    for (const TimingPoint& candidate : m_timingPoints) {
        if (candidate.timeMilliseconds > time) {
            break;
        }
        timingPoint = &candidate;
    }
    const double beatDurationMilliseconds = 60000.0 / timingPoint->beatsPerMinute;
    const double measureDurationMilliseconds = beatDurationMilliseconds
        * timingPoint->timeSignatureNumerator * 4.0
        / std::max(timingPoint->timeSignatureDenominator, 1);
    const double divisorDurationMilliseconds = measureDurationMilliseconds / m_divisor;
    if (divisorDurationMilliseconds <= 0.0) {
        return time;
    }

    const qint64 divisorIndex = static_cast<qint64>(std::llround(
        (time - timingPoint->timeMilliseconds) / divisorDurationMilliseconds));

    return std::max<qint64>(0, timingPoint->timeMilliseconds + static_cast<qint64>(std::llround(
        divisorIndex * divisorDurationMilliseconds)));
}

int FlatView::laneAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const double laneWidth = area.width() / 6.0;
    const int lane = static_cast<int>(std::floor((position.x() - area.left()) / laneWidth));

    return std::clamp(lane, 0, 5);
}

int FlatView::hitObjectAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const double laneWidth = area.width() / 6.0;
    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Tap && hitObject.kind != NoteKind::Hold) {
            continue;
        }
        const qint64 endTime = hitObject.kind == NoteKind::Hold
            ? hitObject.endMilliseconds
            : hitObject.startMilliseconds + kTapDurationMilliseconds;
        const LaneSpan startSpan = laneSpan(hitObject, hitObject.startX, hitObject.startWidth);
        const LaneSpan endSpan = laneSpan(hitObject, hitObject.endX, hitObject.endWidth);
        const QPolygonF object{
            QPointF(area.left() + startSpan.left * laneWidth + 1.0, timeToY(hitObject.startMilliseconds)),
            QPointF(area.left() + startSpan.right * laneWidth - 1.0, timeToY(hitObject.startMilliseconds)),
            QPointF(area.left() + endSpan.right * laneWidth - 1.0, timeToY(endTime)),
            QPointF(area.left() + endSpan.left * laneWidth + 1.0, timeToY(endTime)),
        };
        if (object.containsPoint(position, Qt::OddEvenFill)) {
            return index;
        }
    }

    return -1;
}

int FlatView::holdEndHitObjectAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const double laneWidth = area.width() / 6.0;
    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Hold) {
            continue;
        }
        const LaneSpan endSpan = laneSpan(hitObject, hitObject.endX, hitObject.endWidth);
        const QLineF endEdge(
            QPointF(area.left() + endSpan.left * laneWidth + 1.0, timeToY(hitObject.endMilliseconds)),
            QPointF(area.left() + endSpan.right * laneWidth - 1.0, timeToY(hitObject.endMilliseconds)));
        if (std::abs(position.y() - endEdge.y1()) <= kHoldEndEdgeHitDistance
            && position.x() >= endEdge.x1() - kHoldEndEdgeHitDistance
            && position.x() <= endEdge.x2() + kHoldEndEdgeHitDistance) {
            return index;
        }
    }

    return -1;
}

int FlatView::zoneHitObjectAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const auto skyX = [&area](const double coordinate) {
        return area.left() + std::clamp(coordinate, 0.0, 1.0) * area.width();
    };

    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Sky) {
            continue;
        }
        const qint64 duration = std::max(hitObject.endMilliseconds - hitObject.startMilliseconds, qint64(1));
        QPolygonF leftSide;
        QPolygonF rightSide;
        leftSide.reserve(kZoneSamples + 1);
        rightSide.reserve(kZoneSamples + 1);
        for (int sampleIndex = 0; sampleIndex <= kZoneSamples; ++sampleIndex) {
            const double progress = static_cast<double>(sampleIndex) / kZoneSamples;
            const double left = zoneLeftEdge(hitObject, true)
                + (zoneLeftEdge(hitObject, false) - zoneLeftEdge(hitObject, true))
                    * easingFor(hitObject.auxiliary, false, progress);
            const double right = zoneRightEdge(hitObject, true)
                + (zoneRightEdge(hitObject, false) - zoneRightEdge(hitObject, true))
                    * easingFor(hitObject.auxiliary, true, progress);
            const qint64 time = hitObject.startMilliseconds + static_cast<qint64>(duration * progress);
            leftSide.append(QPointF(skyX(left), timeToY(time)));
            rightSide.append(QPointF(skyX(right), timeToY(time)));
        }
        QPolygonF zone = leftSide;
        for (auto iterator = rightSide.crbegin(); iterator != rightSide.crend(); ++iterator) {
            zone.append(*iterator);
        }
        if (zone.containsPoint(position, Qt::OddEvenFill)) {
            return index;
        }
    }

    return -1;
}

int FlatView::flickHitObjectAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const auto skyX = [&area](const double coordinate) {
        return area.left() + std::clamp(coordinate, 0.0, 1.0) * area.width();
    };

    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Flick) {
            continue;
        }
        const bool pointsRight = (hitObject.auxiliary & kFlickRight) != 0
            || (hitObject.auxiliary & kFlickLeft) == 0;
        const double direction = pointsRight ? 1.0 : -1.0;
        const double width = std::clamp(
            hitObject.startWidth > 0.0 ? hitObject.startWidth : hitObject.endWidth,
            kMinimumFlickTrailWidth,
            1.0);
        const double leadingCoordinate = hitObject.startX + direction * width * 0.5;
        const QPointF leadingPoint(skyX(leadingCoordinate), timeToY(hitObject.startMilliseconds));
        const QPointF fullTrailingPoint(skyX(leadingCoordinate - direction * width), leadingPoint.y());
        const double headLegLength = std::min(flickTriangleHeight(), std::abs(leadingPoint.x() - fullTrailingPoint.x()));
        const double triangleLength = headLegLength / area.width();
        const double trailingCoordinate = leadingCoordinate - direction * triangleLength;
        const QPointF trailingPoint(skyX(trailingCoordinate), leadingPoint.y());
        const QPolygonF triangle{
            trailingPoint,
            leadingPoint,
            trailingPoint + QPointF(0.0, -headLegLength),
        };
        if (triangle.containsPoint(position, Qt::OddEvenFill)) {
            return index;
        }

        const double trailLength = std::max(0.0, width - triangleLength);
        QPolygonF trail;
        trail.reserve(kFlickTrailSamples + 3);
        for (int sampleIndex = 0; sampleIndex <= kFlickTrailSamples; ++sampleIndex) {
            const double progress = static_cast<double>(sampleIndex) / kFlickTrailSamples;
            const double coordinate = trailingCoordinate - direction * trailLength * progress;
            trail.append(QPointF(skyX(coordinate), leadingPoint.y())
                + QPointF(0.0, -headLegLength * (1.0 - std::sqrt(progress))));
        }
        trail.append(trailingPoint);
        if (trail.containsPoint(position, Qt::OddEvenFill)) {
            return index;
        }
    }

    return -1;
}

QVector<int> FlatView::hitObjectsAt(const QPointF& position) const {
    QVector<int> indexes;
    if (canInteractSky()) {
        const int flickIndex = flickHitObjectAt(position);
        const int zoneIndex = zoneHitObjectAt(position);
        if (flickIndex >= 0) {
            indexes.append(flickIndex);
        }
        if (zoneIndex >= 0) {
            indexes.append(zoneIndex);
        }
    }
    if (canInteractGround()) {
        const int groundIndex = hitObjectAt(position);
        if (groundIndex >= 0) {
            indexes.append(groundIndex);
        }
    }

    return indexes;
}

QVector<int> FlatView::hitObjectsInRect(const QRectF& rectangle, const Qt::KeyboardModifiers modifiers) const {
    Q_UNUSED(modifiers)

    const QRectF normalizedRectangle = rectangle.normalized();
    const QRectF area = groundArea();
    const double laneWidth = area.width() / 6.0;
    QVector<int> indexes;

    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        const bool isSkyObject = hitObject.kind == NoteKind::Sky || hitObject.kind == NoteKind::Flick;
        if ((isSkyObject && !canInteractSky()) || (!isSkyObject && !canInteractGround())) {
            continue;
        }

        QRectF hitObjectBounds;
        if (isSkyObject) {
            const SkySpan span = hitObject.kind == NoteKind::Flick
                ? flickSpan(hitObject)
                : SkySpan{
                    .left = std::min(zoneLeftEdge(hitObject, true), zoneLeftEdge(hitObject, false)),
                    .right = std::max(zoneRightEdge(hitObject, true), zoneRightEdge(hitObject, false)),
                };
            const double top = hitObject.kind == NoteKind::Flick
                ? timeToY(hitObject.startMilliseconds) - flickTriangleHeight()
                : timeToY(hitObject.endMilliseconds);
            const double bottom = timeToY(hitObject.startMilliseconds);
            hitObjectBounds = QRectF(
                area.left() + span.left * area.width(),
                std::min(top, bottom),
                (span.right - span.left) * area.width(),
                std::abs(bottom - top));
        } else {
            const LaneSpan startSpan = laneSpan(hitObject, hitObject.startX, hitObject.startWidth);
            const LaneSpan endSpan = laneSpan(hitObject, hitObject.endX, hitObject.endWidth);
            const qint64 endTime = hitObject.kind == NoteKind::Hold
                ? hitObject.endMilliseconds
                : hitObject.startMilliseconds + kTapDurationMilliseconds;
            const double left = area.left() + std::min(startSpan.left, endSpan.left) * laneWidth;
            const double right = area.left() + std::max(startSpan.right, endSpan.right) * laneWidth;
            const double top = timeToY(endTime);
            const double bottom = timeToY(hitObject.startMilliseconds);
            hitObjectBounds = QRectF(left, std::min(top, bottom), right - left, std::abs(bottom - top));
        }
        if (normalizedRectangle.intersects(hitObjectBounds)) {
            indexes.append(index);
        }
    }

    return indexes;
}

QVector<int> FlatView::selectionForClick(const int hitObjectIndex, const Qt::KeyboardModifiers modifiers) const {
    QVector<int> indexes = m_selectedHitObjects;
    if (hitObjectIndex < 0) {
        if ((modifiers & (Qt::ControlModifier | Qt::ShiftModifier)) == 0) {
            indexes.clear();
        }

        return indexes;
    }

    if (modifiers & Qt::ControlModifier) {
        if (indexes.contains(hitObjectIndex)) {
            indexes.removeAll(hitObjectIndex);
        } else {
            indexes.append(hitObjectIndex);
        }

        return indexes;
    }
    if (modifiers & Qt::ShiftModifier) {
        const int anchor = indexes.isEmpty() ? hitObjectIndex : indexes.constLast();
        const int first = std::min(anchor, hitObjectIndex);
        const int last = std::max(anchor, hitObjectIndex);
        for (int index = first; index <= last; ++index) {
            const ChartNote& hitObject = m_chart.notes.at(index);
            const bool isSkyObject = hitObject.kind == NoteKind::Sky || hitObject.kind == NoteKind::Flick;
            if ((isSkyObject ? canInteractSky() : canInteractGround()) && !indexes.contains(index)) {
                indexes.append(index);
            }
        }

        return indexes;
    }

    return {hitObjectIndex};
}

QVector<ChartNote> FlatView::hitObjectsForSelectionMove(const QPointF& position, const bool snapToGrid) const {
    if (m_dragSelectionOriginals.isEmpty()) {
        return {};
    }

    const QRectF area = groundArea();
    const qint64 rawTimeOffset = timeAtY(position.y()) - timeAtY(m_pressPosition.y());
    const int laneOffset = laneAt(position) - laneAt(m_pressPosition);
    const double coordinateOffset = (position.x() - m_pressPosition.x()) / area.width();
    const qint64 firstStart = m_dragSelectionEarliestStart;
    qint64 timeOffset = std::max(rawTimeOffset, -firstStart);
    if (snapToGrid) {
        const qint64 unsnappedStart = firstStart + timeOffset;
        timeOffset = snappedTimeAtY(timeToY(unsnappedStart)) - firstStart;
    }

    QVector<ChartNote> hitObjects;
    hitObjects.reserve(m_dragSelectionOriginals.size());
    for (const ChartNote& original : m_dragSelectionOriginals) {
        ChartNote hitObject = original;
        hitObject.startMilliseconds = std::max<qint64>(0, original.startMilliseconds + timeOffset);
        hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, original.endMilliseconds + timeOffset);
        if (original.kind == NoteKind::Tap || original.kind == NoteKind::Flick) {
            hitObject.endMilliseconds = hitObject.startMilliseconds;
        }

        if (original.kind == NoteKind::Sky) {
            setZoneEndpoint(
                hitObject,
                true,
                zoneLeftEdge(original, true) + coordinateOffset,
                zoneRightEdge(original, true) + coordinateOffset);
            setZoneEndpoint(
                hitObject,
                false,
                zoneLeftEdge(original, false) + coordinateOffset,
                zoneRightEdge(original, false) + coordinateOffset);
        } else if (original.kind == NoteKind::Flick) {
            const SkySpan span = flickSpan(original);
            setFlickSpan(hitObject, span.left + coordinateOffset, span.right + coordinateOffset);
        } else {
            const LaneSpan span = laneSpan(original, original.startX, original.startWidth);
            const int laneCount = std::max(1, static_cast<int>(std::lround(span.right - span.left)));
            const int firstLane = std::clamp(
                static_cast<int>(std::lround(span.left)) + laneOffset,
                0,
                6 - laneCount);
            if (laneCount == 1 && (firstLane == 0 || firstLane == 5)) {
                hitObject.side = floorSideForLane(firstLane);
                hitObject.startX = 0.0;
                hitObject.endX = 0.0;
                hitObject.startWidth = 1.0;
                hitObject.endWidth = 1.0;
            } else {
                const int centralLane = std::clamp(firstLane, 1, 5 - laneCount);
                hitObject.side = FloorSide::Central;
                hitObject.startX = static_cast<double>(centralLane) / 4.0;
                hitObject.endX = hitObject.startX;
                hitObject.startWidth = static_cast<double>(laneCount) / 4.0;
                hitObject.endWidth = hitObject.startWidth;
            }
        }
        hitObjects.append(hitObject);
    }

    return hitObjects;
}

FlatView::FlickControl FlatView::flickControlAt(const QPointF& position) const {
    const QRectF area = groundArea();
    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Flick || !shouldDrawFlickControls(index)) {
            continue;
        }
        const SkySpan span = flickSpan(hitObject);
        const double y = timeToY(hitObject.startMilliseconds);
        const std::array<std::pair<FlickControlKind, QPointF>, 2> controls{{
            {FlickControlKind::Left, QPointF(area.left() + span.left * area.width(), y)},
            {FlickControlKind::Right, QPointF(area.left() + span.right * area.width(), y)},
        }};
        for (const auto& [kind, controlPosition] : controls) {
            if (QLineF(position, controlPosition).length() <= kZoneControlRadius * 1.5) {
                return {.kind = kind, .hitObjectIndex = index};
            }
        }
    }

    return {};
}

FlatView::ZoneControl FlatView::zoneControlAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const auto skyX = [&area](const double coordinate) {
        return area.left() + std::clamp(coordinate, 0.0, 1.0) * area.width();
    };

    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Sky || !shouldDrawZoneControls(index)) {
            continue;
        }
        const qint64 duration = std::max(hitObject.endMilliseconds - hitObject.startMilliseconds, qint64(1));
        const double middleProgress = 0.5;
        const double middleLeft = zoneLeftEdge(hitObject, true)
            + (zoneLeftEdge(hitObject, false) - zoneLeftEdge(hitObject, true))
                * easingFor(hitObject.auxiliary, false, middleProgress);
        const double middleRight = zoneRightEdge(hitObject, true)
            + (zoneRightEdge(hitObject, false) - zoneRightEdge(hitObject, true))
                * easingFor(hitObject.auxiliary, true, middleProgress);
        const qint64 middleTime = hitObject.startMilliseconds + duration / 2;
        const std::array<std::pair<ZoneControlKind, QPointF>, 6> controls{{
            {ZoneControlKind::StartLeft, QPointF(skyX(zoneLeftEdge(hitObject, true)), timeToY(hitObject.startMilliseconds))},
            {ZoneControlKind::StartRight, QPointF(skyX(zoneRightEdge(hitObject, true)), timeToY(hitObject.startMilliseconds))},
            {ZoneControlKind::EndLeft, QPointF(skyX(zoneLeftEdge(hitObject, false)), timeToY(hitObject.endMilliseconds))},
            {ZoneControlKind::EndRight, QPointF(skyX(zoneRightEdge(hitObject, false)), timeToY(hitObject.endMilliseconds))},
            {ZoneControlKind::LeftSide, QPointF(skyX(middleLeft), timeToY(middleTime))},
            {ZoneControlKind::RightSide, QPointF(skyX(middleRight), timeToY(middleTime))},
        }};
        for (const auto& [kind, controlPosition] : controls) {
            if (QLineF(position, controlPosition).length() <= kZoneControlRadius * 1.5) {
                return {.kind = kind, .hitObjectIndex = index};
            }
        }
    }

    return {};
}

FlatView::ZoneSegmentAnchor FlatView::zoneSegmentAnchorAt(const QPointF& position) const {
    const QRectF area = groundArea();
    const auto skyX = [&area](const double coordinate) {
        return area.left() + std::clamp(coordinate, 0.0, 1.0) * area.width();
    };

    for (int index = m_chart.notes.size() - 1; index >= 0; --index) {
        const ChartNote& hitObject = m_chart.notes.at(index);
        if (hitObject.kind != NoteKind::Sky) {
            continue;
        }
        for (const bool atStart : {true, false}) {
            const double left = skyX(zoneLeftEdge(hitObject, atStart));
            const double right = skyX(zoneRightEdge(hitObject, atStart));
            const qint64 time = atStart ? hitObject.startMilliseconds : hitObject.endMilliseconds;
            const double y = timeToY(time);
            if (std::abs(position.y() - y) <= kZoneEdgeHitDistance
                && position.x() >= left - kZoneEdgeHitDistance
                && position.x() <= right + kZoneEdgeHitDistance) {
                return {.hitObjectIndex = index, .atStart = atStart};
            }
        }
    }

    return {};
}

ChartNote FlatView::hitObjectForPlacement(const QPointF& position, const bool snapToGrid) const {
    if (m_pressModifiers & Qt::ShiftModifier) {
        return flickHitObjectForDrag(position, snapToGrid);
    }
    if (m_pressModifiers & Qt::AltModifier) {
        return zoneHitObjectForDrag(position, snapToGrid);
    }

    return floorHitObjectForDrag(position, snapToGrid);
}

ChartNote FlatView::floorHitObjectForDrag(const QPointF& position, const bool snapToGrid) const {
    const int pressLane = laneAt(m_pressPosition);
    const int releaseLane = laneAt(position);
    const qint64 pressTime = snapToGrid ? snappedTimeAtY(m_pressPosition.y()) : timeAtY(m_pressPosition.y());
    const qint64 releaseTime = snapToGrid ? snappedTimeAtY(position.y()) : timeAtY(position.y());
    const qint64 startTime = std::min(pressTime, releaseTime);
    const qint64 endTime = std::max(pressTime, releaseTime);
    ChartNote hitObject;

    hitObject.kind = endTime - startTime >= kMinimumHoldDurationMilliseconds ? NoteKind::Hold : NoteKind::Tap;
    hitObject.startMilliseconds = startTime;
    hitObject.endMilliseconds = hitObject.kind == NoteKind::Hold ? endTime : startTime;
    if (pressLane == 0 || pressLane == 5 || releaseLane == 0 || releaseLane == 5) {
        // An outer lane cannot form a multi-lane floor span. When a drag
        // crosses from a central lane into an outer lane, use the outer lane
        // rather than retaining the central side with outer co-ordinates.
        // The latter serializes as central X=0, which the game indexes before
        // the first central lane.
        const int outerLane = pressLane == 0 || pressLane == 5 ? pressLane : releaseLane;
        hitObject.side = floorSideForLane(outerLane);
        hitObject.startX = 0.0;
        hitObject.endX = 0.0;
        hitObject.startWidth = 1.0;
        hitObject.endWidth = 1.0;

        return hitObject;
    }

    const int firstLane = std::min(pressLane, releaseLane);
    const int laneCount = std::max(1, std::abs(releaseLane - pressLane) + 1);
    hitObject.side = FloorSide::Central;
    hitObject.startX = static_cast<double>(firstLane) / 4.0;
    hitObject.endX = hitObject.startX;
    hitObject.startWidth = static_cast<double>(laneCount) / 4.0;
    hitObject.endWidth = hitObject.startWidth;

    return hitObject;
}

ChartNote FlatView::floorHitObjectForMove(const QPointF& position, const bool snapToGrid) const {
    ChartNote hitObject = m_dragOriginal;
    const qint64 timeOffset = timeAtY(position.y()) - timeAtY(m_pressPosition.y());
    const int targetLane = laneAt(position);
    const LaneSpan originalSpan = laneSpan(m_dragOriginal, m_dragOriginal.startX, m_dragOriginal.startWidth);
    const int laneCount = std::max(1, static_cast<int>(std::lround(originalSpan.right - originalSpan.left)));

    hitObject.startMilliseconds = std::max<qint64>(0, m_dragOriginal.startMilliseconds + timeOffset);
    if (hitObject.kind == NoteKind::Hold) {
        hitObject.endMilliseconds = std::max(
            hitObject.startMilliseconds + kMinimumHoldDurationMilliseconds,
            m_dragOriginal.endMilliseconds + timeOffset);
    } else {
        hitObject.endMilliseconds = hitObject.startMilliseconds;
    }
    if (snapToGrid) {
        const double startY = timeToY(hitObject.startMilliseconds);
        const qint64 snappedStart = snappedTimeAtY(startY);
        const qint64 snapOffset = snappedStart - hitObject.startMilliseconds;
        hitObject.startMilliseconds = snappedStart;
        if (hitObject.kind == NoteKind::Hold) {
            hitObject.endMilliseconds = std::max(
                hitObject.startMilliseconds + kMinimumHoldDurationMilliseconds,
                hitObject.endMilliseconds + snapOffset);
        } else {
            hitObject.endMilliseconds = hitObject.startMilliseconds;
        }
    }
    if (laneCount == 1 && (targetLane == 0 || targetLane == 5)) {
        hitObject.side = floorSideForLane(targetLane);
        hitObject.startX = 0.0;
        hitObject.endX = 0.0;
        hitObject.startWidth = 1.0;
        hitObject.endWidth = 1.0;

        return hitObject;
    }

    const int firstLane = std::clamp(targetLane, 1, 5 - laneCount);
    hitObject.side = FloorSide::Central;
    hitObject.startX = static_cast<double>(firstLane) / 4.0;
    hitObject.endX = hitObject.startX;
    hitObject.startWidth = static_cast<double>(laneCount) / 4.0;
    hitObject.endWidth = hitObject.startWidth;

    return hitObject;
}

ChartNote FlatView::holdHitObjectForEndMove(const QPointF& position, const bool snapToGrid) const {
    ChartNote hitObject = m_dragOriginal;
    const qint64 requestedEnd = snapToGrid ? snappedTimeAtY(position.y()) : timeAtY(position.y());

    hitObject.endMilliseconds = std::max(hitObject.startMilliseconds, requestedEnd);

    return hitObject;
}

ChartNote FlatView::flickHitObjectForDrag(const QPointF& position, const bool snapToGrid) const {
    const QRectF area = groundArea();
    const double pressCoordinate = std::clamp((m_pressPosition.x() - area.left()) / area.width(), 0.0, 1.0);
    const double releaseCoordinate = std::clamp((position.x() - area.left()) / area.width(), 0.0, 1.0);
    const bool pointsRight = releaseCoordinate >= pressCoordinate;
    ChartNote hitObject;

    hitObject.kind = NoteKind::Flick;
    hitObject.side = FloorSide::Sky;
    hitObject.startMilliseconds = snapToGrid ? snappedTimeAtY(m_pressPosition.y()) : timeAtY(m_pressPosition.y());
    hitObject.endMilliseconds = hitObject.startMilliseconds;
    hitObject.startX = (pressCoordinate + releaseCoordinate) * 0.5;
    hitObject.endX = hitObject.startX;
    hitObject.startWidth = std::max(kMinimumFlickWidth, std::abs(releaseCoordinate - pressCoordinate));
    hitObject.endWidth = hitObject.startWidth;
    hitObject.auxiliary = pointsRight ? kFlickRight : kFlickLeft;

    return hitObject;
}

ChartNote FlatView::flickHitObjectForMove(const QPointF& position, const bool snapToGrid) const {
    ChartNote hitObject = m_dragOriginal;
    const QRectF area = groundArea();
    const qint64 timeOffset = timeAtY(position.y()) - timeAtY(m_pressPosition.y());
    const double requestedCoordinateOffset = (position.x() - m_pressPosition.x()) / area.width();
    const SkySpan originalSpan = flickSpan(hitObject);
    const double coordinateOffset = std::clamp(
        requestedCoordinateOffset,
        -originalSpan.left,
        1.0 - originalSpan.right);

    hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds + timeOffset);
    hitObject.endMilliseconds = hitObject.startMilliseconds;
    if (snapToGrid) {
        hitObject.startMilliseconds = snappedTimeAtY(timeToY(hitObject.startMilliseconds));
        hitObject.endMilliseconds = hitObject.startMilliseconds;
    }
    hitObject.startX += coordinateOffset;
    hitObject.endX += coordinateOffset;

    return hitObject;
}

ChartNote FlatView::flickHitObjectForControlMove(const QPointF& position) const {
    ChartNote hitObject = m_dragOriginal;
    const QRectF area = groundArea();
    const double coordinate = std::clamp((position.x() - area.left()) / area.width(), 0.0, 1.0);
    const SkySpan originalSpan = flickSpan(m_dragOriginal);
    const double left = m_dragFlickControl.kind == FlickControlKind::Left
        ? std::min(coordinate, originalSpan.right - kMinimumFlickWidth)
        : originalSpan.left;
    const double right = m_dragFlickControl.kind == FlickControlKind::Right
        ? std::max(coordinate, originalSpan.left + kMinimumFlickWidth)
        : originalSpan.right;
    setFlickSpan(hitObject, left, right);

    return hitObject;
}

ChartNote FlatView::zoneHitObjectForDrag(const QPointF& position, const bool snapToGrid) const {
    const QRectF area = groundArea();
    const double pressCoordinate = std::clamp((m_pressPosition.x() - area.left()) / area.width(), 0.0, 1.0);
    const double releaseCoordinate = std::clamp((position.x() - area.left()) / area.width(), 0.0, 1.0);
    const qint64 pressTime = snapToGrid ? snappedTimeAtY(m_pressPosition.y()) : timeAtY(m_pressPosition.y());
    const qint64 releaseTime = snapToGrid ? snappedTimeAtY(position.y()) : timeAtY(position.y());
    ChartNote hitObject;

    hitObject.kind = NoteKind::Sky;
    hitObject.side = FloorSide::Sky;
    const bool forward = pressTime <= releaseTime;
    hitObject.startMilliseconds = std::min(pressTime, releaseTime);
    hitObject.endMilliseconds = std::max(pressTime, releaseTime);
    hitObject.startX = forward ? pressCoordinate : releaseCoordinate;
    hitObject.endX = forward ? releaseCoordinate : pressCoordinate;
    hitObject.startWidth = kDefaultZoneWidth;
    hitObject.endWidth = kDefaultZoneWidth;
    hitObject.auxiliary = kZoneLinear;

    if (m_zoneSegmentAnchor.hitObjectIndex >= 0
        && m_zoneSegmentAnchor.hitObjectIndex < m_chart.notes.size()) {
        const ChartNote& source = m_chart.notes.at(m_zoneSegmentAnchor.hitObjectIndex);
        const qint64 anchorTime = m_zoneSegmentAnchor.atStart
            ? source.startMilliseconds
            : source.endMilliseconds;
        const double anchorX = m_zoneSegmentAnchor.atStart ? source.startX : source.endX;
        const double anchorWidth = m_zoneSegmentAnchor.atStart ? source.startWidth : source.endWidth;
        const bool anchorIsStart = anchorTime <= releaseTime;
        hitObject.startMilliseconds = std::min(anchorTime, releaseTime);
        hitObject.endMilliseconds = std::max(anchorTime, releaseTime);
        hitObject.startX = anchorIsStart ? anchorX : releaseCoordinate;
        hitObject.endX = anchorIsStart ? releaseCoordinate : anchorX;
        hitObject.startWidth = anchorWidth;
        hitObject.endWidth = anchorWidth;
        hitObject.groupId = source.groupId;
    }

    return hitObject;
}

bool FlatView::zoneExtensionExists(const QPointF& position, const bool snapToGrid) const {
    if (m_zoneSegmentAnchor.hitObjectIndex < 0
        || m_zoneSegmentAnchor.hitObjectIndex >= m_chart.notes.size()) {
        return false;
    }

    const ChartNote& source = m_chart.notes.at(m_zoneSegmentAnchor.hitObjectIndex);
    const qint64 anchorTime = m_zoneSegmentAnchor.atStart ? source.startMilliseconds : source.endMilliseconds;
    const qint64 releaseTime = snapToGrid ? snappedTimeAtY(position.y()) : timeAtY(position.y());
    const bool extendsForward = releaseTime > anchorTime;
    const double anchorLeft = zoneLeftEdge(source, m_zoneSegmentAnchor.atStart);
    const double anchorRight = zoneRightEdge(source, m_zoneSegmentAnchor.atStart);
    constexpr qint64 kTimeToleranceMilliseconds = 1;
    constexpr double kEdgeTolerance = 0.0001;

    for (int index = 0; index < m_chart.notes.size(); ++index) {
        if (index == m_dragHitObjectIndex) {
            continue;
        }
        const ChartNote& candidate = m_chart.notes.at(index);
        if (candidate.kind != NoteKind::Sky || candidate.groupId != source.groupId) {
            continue;
        }
        const bool candidateStartsAtJoint = std::abs(candidate.startMilliseconds - anchorTime) <= kTimeToleranceMilliseconds
            && std::abs(zoneLeftEdge(candidate, true) - anchorLeft) <= kEdgeTolerance
            && std::abs(zoneRightEdge(candidate, true) - anchorRight) <= kEdgeTolerance;
        const bool candidateEndsAtJoint = std::abs(candidate.endMilliseconds - anchorTime) <= kTimeToleranceMilliseconds
            && std::abs(zoneLeftEdge(candidate, false) - anchorLeft) <= kEdgeTolerance
            && std::abs(zoneRightEdge(candidate, false) - anchorRight) <= kEdgeTolerance;
        if ((extendsForward && candidateStartsAtJoint && candidate.endMilliseconds > anchorTime)
            || (!extendsForward && candidateEndsAtJoint && candidate.startMilliseconds < anchorTime)) {
            return true;
        }
    }

    return false;
}

ChartNote FlatView::zoneHitObjectForMove(const QPointF& position, const bool snapToGrid) const {
    ChartNote hitObject = m_dragOriginal;
    const QRectF area = groundArea();
    const qint64 timeOffset = timeAtY(position.y()) - timeAtY(m_pressPosition.y());
    const double requestedCoordinateOffset = (position.x() - m_pressPosition.x()) / area.width();
    const double leftmostEdge = std::min(zoneLeftEdge(hitObject, true), zoneLeftEdge(hitObject, false));
    const double rightmostEdge = std::max(zoneRightEdge(hitObject, true), zoneRightEdge(hitObject, false));
    const double coordinateOffset = std::clamp(
        requestedCoordinateOffset,
        -leftmostEdge,
        1.0 - rightmostEdge);

    hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds + timeOffset);
    hitObject.endMilliseconds = std::max(hitObject.startMilliseconds + 1, hitObject.endMilliseconds + timeOffset);
    hitObject.startX += coordinateOffset;
    hitObject.endX += coordinateOffset;
    bool snappedToZone = false;
    hitObject = snapZoneToAdjacentSegment(hitObject, &snappedToZone);
    if (snappedToZone) {
        return hitObject;
    }
    if (snapToGrid) {
        const qint64 snappedStart = snappedTimeAtY(timeToY(hitObject.startMilliseconds));
        const qint64 snapOffset = snappedStart - hitObject.startMilliseconds;
        hitObject.startMilliseconds = snappedStart;
        hitObject.endMilliseconds = std::max(hitObject.startMilliseconds + 1, hitObject.endMilliseconds + snapOffset);
    }

    return hitObject;
}

ChartNote FlatView::snapZoneToAdjacentSegment(ChartNote hitObject, bool* snapped) const {
    struct Endpoint {
        qint64 timeMilliseconds = 0;
        double left = 0.0;
        double right = 0.0;
        bool atStart = true;
    };

    *snapped = false;
    const std::array<Endpoint, 2> endpoints{{
        {hitObject.startMilliseconds, zoneLeftEdge(hitObject, true), zoneRightEdge(hitObject, true), true},
        {hitObject.endMilliseconds, zoneLeftEdge(hitObject, false), zoneRightEdge(hitObject, false), false},
    }};
    for (int candidateIndex = 0; candidateIndex < m_chart.notes.size(); ++candidateIndex) {
        if (candidateIndex == m_dragHitObjectIndex) {
            continue;
        }
        const ChartNote& candidate = m_chart.notes.at(candidateIndex);
        if (candidate.kind != NoteKind::Sky) {
            continue;
        }
        for (const bool candidateAtStart : {true, false}) {
            const qint64 candidateTime = candidateAtStart ? candidate.startMilliseconds : candidate.endMilliseconds;
            const double candidateLeft = zoneLeftEdge(candidate, candidateAtStart);
            const double candidateRight = zoneRightEdge(candidate, candidateAtStart);
            for (const Endpoint& endpoint : endpoints) {
                if (std::abs(endpoint.timeMilliseconds - candidateTime) > kZoneSnapTimeDistanceMilliseconds
                    || std::abs(endpoint.left - candidateLeft) > kZoneSnapCoordinateDistance
                    || std::abs(endpoint.right - candidateRight) > kZoneSnapCoordinateDistance) {
                    continue;
                }
                const qint64 timeOffset = candidateTime - endpoint.timeMilliseconds;
                const double coordinateOffset = (candidateLeft + candidateRight - endpoint.left - endpoint.right) * 0.5;
                hitObject.startMilliseconds = std::max<qint64>(0, hitObject.startMilliseconds + timeOffset);
                hitObject.endMilliseconds = std::max(hitObject.startMilliseconds + 1, hitObject.endMilliseconds + timeOffset);
                hitObject.startX += coordinateOffset;
                hitObject.endX += coordinateOffset;
                setZoneEndpoint(hitObject, endpoint.atStart, candidateLeft, candidateRight);
                hitObject.groupId = candidate.groupId;
                *snapped = true;

                return hitObject;
            }
        }
    }

    return hitObject;
}

ChartNote FlatView::zoneHitObjectForControlMove(const QPointF& position) const {
    const QVector<ChartNote> hitObjects = zoneHitObjectsForControlMove(position, false);

    return hitObjects.isEmpty() ? m_dragOriginal : hitObjects.front();
}

QVector<int> FlatView::linkedZoneIndexes() const {
    QVector<int> indexes;
    if (m_dragZoneControl.kind == ZoneControlKind::None
        || m_dragHitObjectIndex < 0) {
        return {m_dragHitObjectIndex};
    }
    if (m_dragZoneControl.kind == ZoneControlKind::LeftSide
        || m_dragZoneControl.kind == ZoneControlKind::RightSide) {
        return {m_dragHitObjectIndex};
    }

    const bool controlsStart = m_dragZoneControl.kind == ZoneControlKind::StartLeft
        || m_dragZoneControl.kind == ZoneControlKind::StartRight
        || m_dragZoneControl.kind == ZoneControlKind::StartTime;
    const qint64 sharedTime = controlsStart ? m_dragOriginal.startMilliseconds : m_dragOriginal.endMilliseconds;
    const double sharedLeft = zoneLeftEdge(m_dragOriginal, controlsStart);
    const double sharedRight = zoneRightEdge(m_dragOriginal, controlsStart);
    constexpr qint64 kTimeToleranceMilliseconds = 1;
    constexpr double kEdgeTolerance = 0.0001;

    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& candidate = m_chart.notes.at(index);
        if (candidate.kind != NoteKind::Sky || candidate.groupId != m_dragOriginal.groupId) {
            continue;
        }
        const bool candidateEndpointIsStart = !controlsStart;
        const qint64 candidateTime = candidateEndpointIsStart
            ? candidate.startMilliseconds
            : candidate.endMilliseconds;
        if (std::abs(candidateTime - sharedTime) > kTimeToleranceMilliseconds
            || std::abs(zoneLeftEdge(candidate, candidateEndpointIsStart) - sharedLeft) > kEdgeTolerance
            || std::abs(zoneRightEdge(candidate, candidateEndpointIsStart) - sharedRight) > kEdgeTolerance) {
            continue;
        }
        indexes.append(index);
    }
    if (!indexes.contains(m_dragHitObjectIndex)) {
        indexes.append(m_dragHitObjectIndex);
    }
    std::sort(indexes.begin(), indexes.end());

    return indexes;
}

QVector<ChartNote> FlatView::zoneHitObjectsForControlMove(const QPointF& position, const bool snapToGrid) const {
    QVector<ChartNote> hitObjects = m_dragLinkedZoneOriginals;
    if (hitObjects.isEmpty()) {
        hitObjects.append(m_dragOriginal);
    }
    const QRectF area = groundArea();
    const double coordinate = std::clamp((position.x() - area.left()) / area.width(), 0.0, 1.0);
    const bool controlsStart = m_dragZoneControl.kind == ZoneControlKind::StartLeft
        || m_dragZoneControl.kind == ZoneControlKind::StartRight
        || m_dragZoneControl.kind == ZoneControlKind::StartTime;
    const bool controlsLeft = m_dragZoneControl.kind == ZoneControlKind::StartLeft
        || m_dragZoneControl.kind == ZoneControlKind::EndLeft;

    switch (m_dragZoneControl.kind) {
    case ZoneControlKind::StartLeft:
    case ZoneControlKind::StartRight:
    case ZoneControlKind::EndLeft:
    case ZoneControlKind::EndRight: {
        const double oldLeft = zoneLeftEdge(m_dragOriginal, controlsStart);
        const double oldRight = zoneRightEdge(m_dragOriginal, controlsStart);
        const double newLeft = controlsLeft
            ? std::min(coordinate, oldRight - kMinimumZoneWidth)
            : oldLeft;
        const double newRight = controlsLeft
            ? oldRight
            : std::max(coordinate, oldLeft + kMinimumZoneWidth);
        for (int index = 0; index < hitObjects.size(); ++index) {
            const bool targetIsPrimary = m_dragLinkedZoneIndexes.isEmpty()
                || m_dragLinkedZoneIndexes.value(index) == m_dragHitObjectIndex;
            setZoneEndpoint(hitObjects[index], targetIsPrimary ? controlsStart : !controlsStart, newLeft, newRight);
        }
        break;
    }
    case ZoneControlKind::StartTime:
    case ZoneControlKind::EndTime: {
        const qint64 requestedTime = snapToGrid ? snappedTimeAtY(position.y()) : timeAtY(position.y());
        const qint64 minimumDuration = snapToGrid
            ? std::max<qint64>(m_gridDurationMilliseconds, 1)
            : qint64(1);
        for (int index = 0; index < hitObjects.size(); ++index) {
            const bool targetIsPrimary = m_dragLinkedZoneIndexes.isEmpty()
                || m_dragLinkedZoneIndexes.value(index) == m_dragHitObjectIndex;
            const bool targetControlsStart = targetIsPrimary ? controlsStart : !controlsStart;
            if (targetControlsStart) {
                const qint64 maximumStart = std::max<qint64>(
                    0,
                    hitObjects[index].endMilliseconds - minimumDuration);
                hitObjects[index].startMilliseconds = std::clamp(
                    requestedTime,
                    qint64(0),
                    maximumStart);
                hitObjects[index].endMilliseconds = std::max(
                    hitObjects[index].endMilliseconds,
                    hitObjects[index].startMilliseconds + minimumDuration);
            } else {
                hitObjects[index].endMilliseconds = std::max(
                    requestedTime,
                    hitObjects[index].startMilliseconds + minimumDuration);
            }
        }
        break;
    }
    case ZoneControlKind::LeftSide:
    case ZoneControlKind::RightSide: {
        const bool rightSide = m_dragZoneControl.kind == ZoneControlKind::RightSide;
        const std::uint32_t mask = rightSide ? 0xE0U : 0x1CU;
        const std::uint32_t linear = rightSide ? kZoneLinear & mask : kZoneLinear & mask;
        const std::uint32_t sineOut = rightSide ? kWidthSineOut : kCenterSineOut;
        const std::uint32_t sineIn = rightSide ? kWidthSineIn : kCenterSineIn;
        const double startEdge = rightSide
            ? zoneRightEdge(m_dragOriginal, true)
            : zoneLeftEdge(m_dragOriginal, true);
        const double endEdge = rightSide
            ? zoneRightEdge(m_dragOriginal, false)
            : zoneLeftEdge(m_dragOriginal, false);
        const double midpoint = area.left() + (startEdge + endEdge) * area.width() * 0.5;
        const double edgeDirection = endEdge - startEdge;
        const double cursorOffset = position.x() - midpoint;
        std::uint32_t mode = m_dragOriginal.auxiliary & mask;
        if (m_dragMoved) {
            if (std::abs(cursorOffset) < kMouseDragDistance
                || std::abs(edgeDirection * area.width()) < kMouseDragDistance) {
                mode = linear;
            } else {
                mode = cursorOffset * edgeDirection > 0.0 ? sineOut : sineIn;
            }
        }
        for (ChartNote& hitObject : hitObjects) {
            hitObject.auxiliary = (hitObject.auxiliary & ~mask) | mode;
        }
        break;
    }
    case ZoneControlKind::None:
        break;
    }

    return hitObjects;
}

bool FlatView::canInteractGround() const {
    return m_mode != FlatViewMode::Sky;
}

bool FlatView::canInteractSky() const {
    return m_mode != FlatViewMode::Ground;
}

bool FlatView::canPlaceGround() const {
    return m_tool == EditorTool::Place && canInteractGround();
}

bool FlatView::canPlaceSky() const {
    return m_tool == EditorTool::Place && canInteractSky();
}

QString FlatView::waveformToolPath() const {
    QDir applicationDirectory(QCoreApplication::applicationDirPath());
    applicationDirectory.cdUp();
    applicationDirectory.cdUp();

    return applicationDirectory.filePath(QStringLiteral("tools/audiowaveform/audiowaveform.exe"));
}

bool FlatView::loadWaveform(const QByteArray& waveformData) {
    const QJsonDocument document = QJsonDocument::fromJson(waveformData);
    if (!document.isObject()) {
        return false;
    }
    const QJsonObject root = document.object();
    const QJsonArray data = root.value(QStringLiteral("data")).toArray();
    const int sampleRate = root.value(QStringLiteral("sample_rate")).toInt();
    const int samplesPerPixel = root.value(QStringLiteral("samples_per_pixel")).toInt();
    const int bits = root.value(QStringLiteral("bits")).toInt();
    if (data.size() < 2 || sampleRate <= 0 || samplesPerPixel <= 0 || (data.size() % 2) != 0) {
        return false;
    }

    const double amplitudeRange = bits == 8 ? 128.0 : 32768.0;
    const double millisecondsPerPeak = 1000.0 * samplesPerPixel / sampleRate;
    QVector<QPointF> peaks;
    peaks.reserve(data.size() / 2);
    for (int index = 0; index < data.size(); index += 2) {
        peaks.append({
            data.at(index).toDouble() / amplitudeRange,
            data.at(index + 1).toDouble() / amplitudeRange,
        });
    }
    m_waveformPeaks = std::move(peaks);
    m_waveformMillisecondsPerPeak = millisecondsPerPeak;
    update();

    return true;
}

void FlatView::appendWaveformAudio(const QAudioBuffer& buffer) {
    const QAudioFormat format = buffer.format();
    const int sampleRate = format.sampleRate();
    const int channelCount = format.channelCount();
    if (sampleRate <= 0 || channelCount <= 0 || buffer.frameCount() <= 0) {
        return;
    }

    if (m_waveformFramesInPeak == 0 && m_waveformPeaks.isEmpty()) {
        m_waveformFramesPerPeak = std::max(1, sampleRate / kWaveformPointsPerSecond);
        m_waveformMillisecondsPerPeak = 1000.0 * m_waveformFramesPerPeak / sampleRate;
    }

    const auto sampleAt = [&format, &buffer](const int index) {
        switch (format.sampleFormat()) {
        case QAudioFormat::UInt8:
            return (static_cast<int>(buffer.constData<quint8>()[index]) - 128) / 128.0;
        case QAudioFormat::Int16:
            return buffer.constData<qint16>()[index] / 32768.0;
        case QAudioFormat::Int32:
            return buffer.constData<qint32>()[index] / 2147483648.0;
        case QAudioFormat::Float:
            return static_cast<double>(buffer.constData<float>()[index]);
        default:
            return 0.0;
        }
    };

    for (int frame = 0; frame < buffer.frameCount(); ++frame) {
        double frameMinimum = 1.0;
        double frameMaximum = -1.0;
        for (int channel = 0; channel < channelCount; ++channel) {
            const double sample = std::clamp(sampleAt(frame * channelCount + channel), -1.0, 1.0);
            frameMinimum = std::min(frameMinimum, sample);
            frameMaximum = std::max(frameMaximum, sample);
        }
        if (m_waveformFramesInPeak == 0) {
            m_waveformMinimum = frameMinimum;
            m_waveformMaximum = frameMaximum;
        } else {
            m_waveformMinimum = std::min(m_waveformMinimum, frameMinimum);
            m_waveformMaximum = std::max(m_waveformMaximum, frameMaximum);
        }
        ++m_waveformFramesInPeak;
        if (m_waveformFramesInPeak >= m_waveformFramesPerPeak) {
            m_waveformPeaks.append(QPointF(m_waveformMinimum, m_waveformMaximum));
            m_waveformFramesInPeak = 0;
        }
    }
}

void FlatView::mousePressEvent(QMouseEvent* event) {
    if (!groundArea().contains(event->position())) {
        event->ignore();
        return;
    }

    if (m_tool == EditorTool::Select) {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }

        const QVector<int> candidates = hitObjectsAt(event->position());
        m_pressPosition = event->position();
        m_pressModifiers = event->modifiers();
        m_dragHitObjectIndex = candidates.isEmpty() ? -1 : candidates.front();
        m_dragMoved = false;
        m_marqueeSelecting = true;
        event->accept();
        return;
    }

    if (m_tool == EditorTool::Move) {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }

        const QVector<int> candidates = hitObjectsAt(event->position());
        const auto iterator = std::find_if(candidates.cbegin(), candidates.cend(), [this](const int index) {
            return m_selectedHitObjects.contains(index);
        });
        if (iterator == candidates.cend()) {
            event->accept();
            return;
        }

        m_pressPosition = event->position();
        m_pressModifiers = event->modifiers();
        m_dragMoved = false;
        m_movingSelection = true;
        m_dragSelectionIndexes = m_selectedHitObjects;
        m_dragSelectionOriginals.clear();
        m_dragSelectionOriginals.reserve(m_dragSelectionIndexes.size());
        m_dragSelectionEarliestStart = std::numeric_limits<qint64>::max();
        for (const int index : m_dragSelectionIndexes) {
            const ChartNote& hitObject = m_chart.notes.at(index);
            m_dragSelectionOriginals.append(hitObject);
            m_dragSelectionEarliestStart = std::min(m_dragSelectionEarliestStart, hitObject.startMilliseconds);
        }
        if (m_pressModifiers & Qt::AltModifier) {
            emit hitObjectsCopyRequested(m_dragSelectionOriginals);
            m_dragSelectionIndexes = m_selectedHitObjects;
            m_dragSelectionOriginals.clear();
            m_dragSelectionOriginals.reserve(m_dragSelectionIndexes.size());
            m_dragSelectionEarliestStart = std::numeric_limits<qint64>::max();
            for (const int index : m_dragSelectionIndexes) {
                const ChartNote& hitObject = m_chart.notes.at(index);
                m_dragSelectionOriginals.append(hitObject);
                m_dragSelectionEarliestStart = std::min(m_dragSelectionEarliestStart, hitObject.startMilliseconds);
            }
        }
        emit hitObjectBatchMoveStarted(m_dragSelectionIndexes);
        event->accept();
        return;
    }

    const int groundHitObjectIndex = canInteractGround() ? hitObjectAt(event->position()) : -1;
    if (event->button() == Qt::RightButton) {
        if (m_tool != EditorTool::Place) {
            event->ignore();
            return;
        }
        const int flickHitObjectIndex = canInteractSky() ? flickHitObjectAt(event->position()) : -1;
        const int zoneHitObjectIndex = canInteractSky() ? zoneHitObjectAt(event->position()) : -1;
        const int skyHitObjectIndex = flickHitObjectIndex >= 0 ? flickHitObjectIndex : zoneHitObjectIndex;
        if (canInteractSky() && skyHitObjectIndex >= 0) {
            emit hitObjectRemoveRequested(skyHitObjectIndex);
        } else if (canInteractGround() && groundHitObjectIndex >= 0) {
            emit hitObjectRemoveRequested(groundHitObjectIndex);
        }
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        event->ignore();
        return;
    }

    m_pressPosition = event->position();
    m_pressModifiers = event->modifiers();
    m_dragMoved = false;
    m_draggingHoldEnd = false;
    m_selectionOnlyClick = false;
    // Placement modifiers describe the requested object type. They must win
    // over any existing note, zone, or control under the initial click.
    if (m_tool == EditorTool::Place && (m_pressModifiers & (Qt::AltModifier | Qt::ShiftModifier))
        && canPlaceSky()) {
        m_dragHitObjectIndex = m_chart.notes.size();
        m_draggingHitObject = false;
        m_placingHitObject = true;
        m_dragZoneControl = {};
        m_dragFlickControl = {};
        m_zoneSegmentAnchor = {};
        emit hitObjectAddRequested(hitObjectForPlacement(m_pressPosition, true));
        if (m_dragHitObjectIndex < m_chart.notes.size()) {
            m_dragOriginal = m_chart.notes.at(m_dragHitObjectIndex);
            emit hitObjectMoveStarted(m_dragHitObjectIndex);
        }
        event->accept();
        return;
    }
    m_dragZoneControl = zoneControlAt(event->position());
    m_dragFlickControl = flickControlAt(event->position());
    if (!canInteractSky()) {
        m_dragZoneControl = {};
        m_dragFlickControl = {};
    }
    m_zoneSegmentAnchor = canInteractSky() && (m_pressModifiers & Qt::AltModifier)
        ? zoneSegmentAnchorAt(event->position())
        : ZoneSegmentAnchor{};
    if (m_zoneSegmentAnchor.hitObjectIndex >= 0) {
        m_dragZoneControl = {};
    } else if (canInteractSky() && m_dragZoneControl.kind == ZoneControlKind::None) {
        const ZoneSegmentAnchor timeAnchor = zoneSegmentAnchorAt(event->position());
        if (timeAnchor.hitObjectIndex >= 0) {
            m_dragZoneControl = {
                .kind = timeAnchor.atStart ? ZoneControlKind::StartTime : ZoneControlKind::EndTime,
                .hitObjectIndex = timeAnchor.hitObjectIndex,
            };
        }
    }
    if (m_dragFlickControl.kind != FlickControlKind::None) {
        m_dragZoneControl = {};
        m_zoneSegmentAnchor = {};
    }
    const QVector<int> clickedIndexes = hitObjectsAt(event->position());
    const bool continuesClickCycle = QLineF(event->position(), m_clickCyclePosition).length() <= kMouseDragDistance
        && clickedIndexes == m_clickCycleIndexes;
    if (!continuesClickCycle) {
        m_clickCycleIndexes = clickedIndexes;
        m_clickCyclePosition = event->position();
        m_clickCycleIndex = -1;
    }
    const int cycledHitObjectIndex = m_clickCycleIndexes.isEmpty()
        ? -1
        : m_clickCycleIndexes.at((m_clickCycleIndex + 1) % m_clickCycleIndexes.size());
    const int zoneHitObjectIndex = m_dragZoneControl.kind == ZoneControlKind::None
            && m_zoneSegmentAnchor.hitObjectIndex < 0
            && !(m_pressModifiers & Qt::AltModifier)
        && cycledHitObjectIndex >= 0
        && m_chart.notes.at(cycledHitObjectIndex).kind == NoteKind::Sky
        ? cycledHitObjectIndex
        : -1;
    const int flickHitObjectIndex = m_dragZoneControl.kind == ZoneControlKind::None
            && m_dragFlickControl.kind == FlickControlKind::None
            && zoneHitObjectIndex < 0
            && m_zoneSegmentAnchor.hitObjectIndex < 0
            && !(m_pressModifiers & Qt::AltModifier)
        && cycledHitObjectIndex >= 0
        && m_chart.notes.at(cycledHitObjectIndex).kind == NoteKind::Flick
        ? cycledHitObjectIndex
        : -1;
    const int holdEndHitObjectIndex = m_dragZoneControl.kind == ZoneControlKind::None
            && m_dragFlickControl.kind == FlickControlKind::None
            && zoneHitObjectIndex < 0
            && flickHitObjectIndex < 0
            && m_zoneSegmentAnchor.hitObjectIndex < 0
            && canInteractGround()
        ? holdEndHitObjectAt(event->position())
        : -1;
    m_dragHitObjectIndex = m_dragFlickControl.kind != FlickControlKind::None
        ? m_dragFlickControl.hitObjectIndex
        : m_dragZoneControl.kind != ZoneControlKind::None ? m_dragZoneControl.hitObjectIndex
        : zoneHitObjectIndex >= 0 ? zoneHitObjectIndex
        : flickHitObjectIndex >= 0 ? flickHitObjectIndex
        : m_zoneSegmentAnchor.hitObjectIndex >= 0 ? -1
        : holdEndHitObjectIndex >= 0 ? holdEndHitObjectIndex : cycledHitObjectIndex >= 0 ? cycledHitObjectIndex : groundHitObjectIndex;
    m_draggingHoldEnd = holdEndHitObjectIndex >= 0;
    m_draggingHitObject = m_dragHitObjectIndex >= 0;
    m_placingHitObject = m_dragHitObjectIndex < 0;
    if (m_draggingHitObject) {
        if (m_dragZoneControl.kind == ZoneControlKind::None && m_dragFlickControl.kind == FlickControlKind::None) {
            m_clickCycleIndex = m_clickCycleIndexes.indexOf(m_dragHitObjectIndex);
        }
        m_selectionOnlyClick = !m_selectedHitObjects.contains(m_dragHitObjectIndex);
        emit hitObjectSelectionRequested({m_dragHitObjectIndex});
        m_dragOriginal = m_chart.notes.at(m_dragHitObjectIndex);
        if (m_selectionOnlyClick || m_tool == EditorTool::Select) {
            m_selectionOnlyClick = true;
            event->accept();
            return;
        }
        if (m_dragZoneControl.kind != ZoneControlKind::None) {
            m_dragLinkedZoneIndexes = linkedZoneIndexes();
            m_dragLinkedZoneOriginals.clear();
            m_dragLinkedZoneOriginals.reserve(m_dragLinkedZoneIndexes.size());
            for (const int index : m_dragLinkedZoneIndexes) {
                m_dragLinkedZoneOriginals.append(m_chart.notes.at(index));
            }
            emit hitObjectBatchMoveStarted(m_dragLinkedZoneIndexes);
        } else {
            emit hitObjectMoveStarted(m_dragHitObjectIndex);
        }
    } else if (m_tool == EditorTool::Place
        && ((m_pressModifiers & (Qt::AltModifier | Qt::ShiftModifier)) ? canPlaceSky() : canPlaceGround())) {
        m_dragHitObjectIndex = m_chart.notes.size();
        emit hitObjectAddRequested(hitObjectForPlacement(m_pressPosition, true));
        if (m_dragHitObjectIndex < m_chart.notes.size()) {
            m_dragOriginal = m_chart.notes.at(m_dragHitObjectIndex);
            emit hitObjectMoveStarted(m_dragHitObjectIndex);
        }
    } else {
        event->ignore();
        return;
    }
    event->accept();
}

void FlatView::mouseMoveEvent(QMouseEvent* event) {
    if (m_marqueeSelecting) {
        if (!(event->buttons() & Qt::LeftButton)) {
            event->ignore();
            return;
        }
        m_dragMoved = (event->position() - m_pressPosition).manhattanLength() >= kMouseDragDistance;
        update();
        event->accept();
        return;
    }

    if (m_movingSelection) {
        if (!(event->buttons() & Qt::LeftButton)) {
            event->ignore();
            return;
        }
        m_dragMoved = (event->position() - m_pressPosition).manhattanLength() >= kMouseDragDistance;
        if (m_dragMoved) {
            emit hitObjectBatchMoveRequested(m_dragSelectionIndexes, hitObjectsForSelectionMove(event->position(), false));
        }
        event->accept();
        return;
    }

    if (!(event->buttons() & Qt::LeftButton) || (!m_draggingHitObject && !m_placingHitObject)) {
        event->ignore();
        return;
    }

    if (m_selectionOnlyClick) {
        event->accept();
        return;
    }

    if ((event->position() - m_pressPosition).manhattanLength() >= kMouseDragDistance) {
        m_dragMoved = true;
    }
    if (m_draggingHitObject && m_dragZoneControl.kind != ZoneControlKind::None) {
        emit hitObjectBatchMoveRequested(m_dragLinkedZoneIndexes, zoneHitObjectsForControlMove(event->position(), false));
    } else if (m_draggingHitObject && m_dragFlickControl.kind != FlickControlKind::None) {
        emit hitObjectMoveRequested(m_dragHitObjectIndex, flickHitObjectForControlMove(event->position()));
    } else if (m_draggingHitObject && m_draggingHoldEnd && m_dragMoved) {
        emit hitObjectMoveRequested(m_dragHitObjectIndex, holdHitObjectForEndMove(event->position(), false));
    } else if (m_draggingHitObject && m_dragOriginal.kind == NoteKind::Sky && m_dragMoved) {
        emit hitObjectMoveRequested(m_dragHitObjectIndex, zoneHitObjectForMove(event->position(), false));
    } else if (m_draggingHitObject && m_dragOriginal.kind == NoteKind::Flick && m_dragMoved) {
        emit hitObjectMoveRequested(m_dragHitObjectIndex, flickHitObjectForMove(event->position(), false));
    } else if (m_draggingHitObject && m_dragMoved) {
        emit hitObjectMoveRequested(m_dragHitObjectIndex, floorHitObjectForMove(event->position(), false));
    } else if (m_placingHitObject) {
        emit hitObjectMoveRequested(m_dragHitObjectIndex, hitObjectForPlacement(event->position(), false));
    }
    event->accept();
}

void FlatView::mouseReleaseEvent(QMouseEvent* event) {
    if (m_marqueeSelecting) {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }

        if (m_dragMoved) {
            QVector<int> indexes = hitObjectsInRect(QRectF(m_pressPosition, event->position()), m_pressModifiers);
            if (m_pressModifiers & Qt::ControlModifier) {
                QVector<int> combined = m_selectedHitObjects;
                for (const int index : indexes) {
                    if (combined.contains(index)) {
                        combined.removeAll(index);
                    } else {
                        combined.append(index);
                    }
                }
                indexes = combined;
            } else if (m_pressModifiers & Qt::ShiftModifier) {
                indexes += m_selectedHitObjects;
            }
            emit hitObjectSelectionRequested(indexes);
        } else {
            emit hitObjectSelectionRequested(selectionForClick(m_dragHitObjectIndex, m_pressModifiers));
        }
        m_dragHitObjectIndex = -1;
        m_marqueeSelecting = false;
        m_dragMoved = false;
        update();
        event->accept();
        return;
    }

    if (m_movingSelection) {
        if (event->button() != Qt::LeftButton) {
            event->ignore();
            return;
        }

        if (m_dragMoved) {
            emit hitObjectBatchMoveRequested(m_dragSelectionIndexes, hitObjectsForSelectionMove(event->position(), true));
        }
        emit hitObjectBatchMoveFinished();
        m_dragSelectionIndexes.clear();
        m_dragSelectionOriginals.clear();
        m_dragSelectionEarliestStart = 0;
        m_dragMoved = false;
        m_movingSelection = false;
        event->accept();
        return;
    }

    if (event->button() != Qt::LeftButton || (!m_draggingHitObject && !m_placingHitObject)) {
        event->ignore();
        return;
    }

    if (m_draggingHitObject) {
        if (m_selectionOnlyClick) {
            // The initial press only establishes selection.
        } else if (m_dragZoneControl.kind != ZoneControlKind::None) {
            emit hitObjectBatchMoveRequested(m_dragLinkedZoneIndexes, zoneHitObjectsForControlMove(event->position(), true));
            emit hitObjectBatchMoveFinished();
        } else if (m_dragFlickControl.kind != FlickControlKind::None) {
            emit hitObjectMoveRequested(m_dragHitObjectIndex, flickHitObjectForControlMove(event->position()));
            emit hitObjectMoveFinished();
        } else if (m_draggingHoldEnd && m_dragMoved) {
            emit hitObjectMoveRequested(m_dragHitObjectIndex, holdHitObjectForEndMove(event->position(), true));
            emit hitObjectMoveFinished();
        } else if (m_dragOriginal.kind == NoteKind::Sky && m_dragMoved) {
            emit hitObjectMoveRequested(m_dragHitObjectIndex, zoneHitObjectForMove(event->position(), true));
            emit hitObjectMoveFinished();
        } else if (m_dragOriginal.kind == NoteKind::Flick && m_dragMoved) {
            emit hitObjectMoveRequested(m_dragHitObjectIndex, flickHitObjectForMove(event->position(), true));
            emit hitObjectMoveFinished();
        } else if (m_dragMoved) {
            emit hitObjectMoveRequested(m_dragHitObjectIndex, floorHitObjectForMove(event->position(), true));
            emit hitObjectMoveFinished();
        }
        if (m_dragZoneControl.kind == ZoneControlKind::None && !m_dragMoved) {
            emit hitObjectMoveFinished();
        }
    } else {
        const ChartNote hitObject = hitObjectForPlacement(event->position(), true);
        if (hitObject.kind == NoteKind::Sky
            && (hitObject.endMilliseconds <= hitObject.startMilliseconds
                || zoneExtensionExists(event->position(), true))) {
            emit hitObjectCreationCancelled(m_dragHitObjectIndex);
        } else {
            emit hitObjectMoveRequested(m_dragHitObjectIndex, hitObject);
            emit hitObjectMoveFinished();
        }
    }
    m_dragHitObjectIndex = -1;
    m_draggingHitObject = false;
    m_placingHitObject = false;
    m_selectionOnlyClick = false;
    m_draggingHoldEnd = false;
    m_dragZoneControl = {};
    m_dragFlickControl = {};
    m_zoneSegmentAnchor = {};
    m_dragLinkedZoneIndexes.clear();
    m_dragLinkedZoneOriginals.clear();
    event->accept();
}

void FlatView::paintGL() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette::canvasBackground);

    const QRectF content = rect().adjusted(kMargin, kMargin, -kMargin, -kMargin);
    const QRectF laneArea = groundArea();
    const QRectF waveformArea(content.left(), content.top(), laneArea.left() - content.left() - 6.0, content.height());
    drawWaveform(painter, waveformArea);

    const int groundOpacity = m_mode == FlatViewMode::Sky ? kInactiveOpacity : 255;
    const int skyOpacity = m_mode == FlatViewMode::Ground ? kInactiveOpacity : 255;
    drawGround(painter, laneArea, groundOpacity);
    drawSky(painter, laneArea, skyOpacity);

    painter.setPen(QPen(palette::outerNoteRed, 2.0));
    painter.drawLine(QPointF(content.left(), playheadY()), QPointF(content.right(), playheadY()));
    painter.setPen(palette::textMuted);
    painter.drawText(
        QRectF(content.left(), content.top(), content.width(), 18.0),
        Qt::AlignRight | Qt::AlignTop,
        QStringLiteral("%1 px/s").arg(m_pixelsPerSecond, 0, 'f', 0));
    drawSelectionRectangle(painter);
}

void FlatView::wheelEvent(QWheelEvent* event) {
    double steps = static_cast<double>(event->angleDelta().y()) / 120.0;
    if (std::abs(steps) < 0.001 && !event->pixelDelta().isNull()) {
        steps = static_cast<double>(event->pixelDelta().y()) / 120.0;
    }
    if (std::abs(steps) < 0.001) {
        event->ignore();
        return;
    }

    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        if (steps > 0.0) {
            emit divisorIncreaseRequested();
        } else {
            emit divisorDecreaseRequested();
        }
    } else if ((event->modifiers() & Qt::AltModifier) != 0) {
        zoom(steps);
    } else {
        const int direction = steps > 0.0 ? -1 : 1;
        const int count = std::max(1, static_cast<int>(std::lround(std::abs(steps))));
        for (int index = 0; index < count; ++index) {
            emit divisorSeekRequested(direction);
        }
    }
    event->accept();
}

double FlatView::timeToY(const qint64 timeMilliseconds) const {
    return playheadY() - static_cast<double>(timeMilliseconds - m_playbackPositionMilliseconds) * m_pixelsPerSecond / 1000.0;
}

double FlatView::playheadY() const {
    return kMargin + static_cast<double>(height() - 2.0 * kMargin) * 0.74;
}

double FlatView::flickTriangleHeight() const {
    return kFlickTriangleHeight * m_pixelsPerSecond / kMaximumPixelsPerSecond;
}

void FlatView::drawSelectionRectangle(QPainter& painter) const {
    if (!m_marqueeSelecting || !m_dragMoved) {
        return;
    }

    painter.setPen(QPen(palette::guideYellow, 1.0, Qt::DashLine));
    painter.setBrush(palette::withAlpha(palette::guideYellow, 28));
    painter.drawRect(QRectF(m_pressPosition, mapFromGlobal(QCursor::pos())).normalized());
}

void FlatView::drawWaveform(QPainter& painter, const QRectF& area) const {
    painter.setPen(QPen(palette::gridLine, 1.0));
    painter.setBrush(palette::panel);
    painter.drawRect(area);
    if (m_waveformPeaks.isEmpty()) {
        return;
    }

    const qint64 visibleStartMilliseconds = std::max<qint64>(0, timeAtY(area.bottom()));
    const qint64 visibleEndMilliseconds = std::max<qint64>(0, timeAtY(area.top()));
    const double millisecondsPerScreenPixel = 1000.0 / m_pixelsPerSecond;
    const int waveformPeakCount = static_cast<int>(m_waveformPeaks.size());
    const int peaksPerEnvelope = std::max(1, static_cast<int>(std::floor(
        millisecondsPerScreenPixel / m_waveformMillisecondsPerPeak)));
    const double envelopeDurationMilliseconds = peaksPerEnvelope * m_waveformMillisecondsPerPeak;
    const int firstEnvelope = std::max(0, static_cast<int>(std::floor(
        visibleStartMilliseconds / envelopeDurationMilliseconds)));
    const int lastEnvelope = std::min(
        (waveformPeakCount - 1) / peaksPerEnvelope,
        static_cast<int>(std::ceil(visibleEndMilliseconds / envelopeDurationMilliseconds)));

    const double centerX = area.center().x();
    painter.setPen(QPen(palette::floorNoteBlue, 1.0));
    for (int envelopeIndex = firstEnvelope; envelopeIndex <= lastEnvelope; ++envelopeIndex) {
        const int firstPeak = envelopeIndex * peaksPerEnvelope;
        const int lastPeak = std::min(firstPeak + peaksPerEnvelope, waveformPeakCount);
        const qint64 envelopeMiddleMilliseconds = static_cast<qint64>(std::llround(
            (firstPeak + lastPeak) * m_waveformMillisecondsPerPeak * 0.5));
        const double y = timeToY(envelopeMiddleMilliseconds);
        if (y < area.top() - 1.0 || y > area.bottom() + 1.0) {
            continue;
        }

        double minimumAmplitude = 1.0;
        double maximumAmplitude = -1.0;
        for (int peakIndex = firstPeak; peakIndex < lastPeak; ++peakIndex) {
            const QPointF peak = m_waveformPeaks.at(peakIndex);
            minimumAmplitude = std::min(minimumAmplitude, peak.x());
            maximumAmplitude = std::max(maximumAmplitude, peak.y());
        }
        painter.drawLine(
            QPointF(centerX + minimumAmplitude * area.width() * 0.42, y),
            QPointF(centerX + maximumAmplitude * area.width() * 0.42, y));
    }
}

void FlatView::drawDividers(QPainter& painter, const QRectF& area) const {
    if (m_timingPoints.isEmpty()) {
        return;
    }

    const qint64 visibleStart = timeAtY(area.bottom());
    const qint64 visibleEnd = timeAtY(area.top());
    for (int pointIndex = 0; pointIndex < m_timingPoints.size(); ++pointIndex) {
        const TimingPoint& point = m_timingPoints.at(pointIndex);
        const qint64 sectionStart = point.timeMilliseconds;
        const qint64 sectionEnd = pointIndex + 1 < m_timingPoints.size()
            ? m_timingPoints.at(pointIndex + 1).timeMilliseconds
            : std::max(m_chart.durationMilliseconds, visibleEnd);
        const double beatDuration = 60000.0 / point.beatsPerMinute;
        const double measureDuration = beatDuration * point.timeSignatureNumerator * 4.0
            / std::max(point.timeSignatureDenominator, 1);
        if (measureDuration <= 0.0 || sectionEnd < visibleStart || sectionStart > visibleEnd) {
            continue;
        }

        const qint64 firstMeasure = std::max<qint64>(
            0,
            static_cast<qint64>(std::floor((visibleStart - sectionStart) / measureDuration)));
        for (qint64 measureIndex = firstMeasure;; ++measureIndex) {
            const double measureStart = sectionStart + measureIndex * measureDuration;
            if (measureStart >= sectionEnd || measureStart > visibleEnd) {
                break;
            }
            for (int subdivision = 0; subdivision < m_divisor; ++subdivision) {
                const double time = measureStart + measureDuration * subdivision / m_divisor;
                if (time < visibleStart || time > visibleEnd || time >= sectionEnd) {
                    continue;
                }

                QColor color = palette::textMuted;
                double width = 0.25;
                if (subdivision == 0) {
                    color = palette::white;
                    width = 0.5;
                } else if (m_divisor % 2 == 0 && subdivision == m_divisor / 2) {
                    color = palette::outerNoteRed;
                } else if (m_divisor % 4 == 0 && subdivision % (m_divisor / 4) == 0) {
                    color = palette::floorNoteBlue;
                } else if (m_divisor % 3 == 0 && subdivision % (m_divisor / 3) == 0) {
                    color = palette::guideYellow;
                } else if (m_divisor % 6 == 0 && subdivision % (m_divisor / 6) == 0) {
                    color = palette::outerNotePurple;
                }
                const double y = timeToY(static_cast<qint64>(std::llround(time)));
                // Fractional-width stroked lines are inconsistent across the
                // raster and OpenGL QPainter engines. An aligned strip keeps
                // the paused grid crisp on either backing surface.
                const double thickness = subdivision == 0 ? 2.0 : std::max(1.0, width);
                const double alignedY = std::floor(y) + 0.5;
                painter.fillRect(QRectF(area.left(), alignedY - thickness * 0.5, area.width(), thickness), color);
            }
        }
    }
}

void FlatView::drawGround(QPainter& painter, const QRectF& area, const int opacity) const {
    const double laneWidth = area.width() / 6.0;
    painter.setPen(QPen(palette::gridLine, 1.0));
    painter.setBrush(palette::withAlpha(palette::chrome, opacity));
    painter.drawRect(area);
    // An OFF event opens a lane-dimming zone and the next ON event for that
    // lane closes it. Draw the chart-time span, rather than treating it as an
    // immediate full-view state, so the zone reads like a Hold in the editor.
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::withAlpha(palette::canvasBackground.darker(180), std::min(210, opacity + 90)));
    for (int lane = 0; lane < 6; ++lane) {
        bool dimming = false;
        qint64 zoneStart = 0;
        for (const LaneEvent& event : m_laneEvents) {
            if (event.lane != lane) {
                continue;
            }
            if (!event.enabled && !dimming) {
                zoneStart = event.timeMilliseconds;
                dimming = true;
            } else if (event.enabled && dimming) {
                const double firstY = timeToY(zoneStart);
                const double lastY = timeToY(event.timeMilliseconds);
                painter.drawRect(QRectF(area.left() + lane * laneWidth, std::min(firstY, lastY), laneWidth,
                    std::abs(lastY - firstY)));
                dimming = false;
            }
        }
        if (dimming) {
            const double firstY = timeToY(zoneStart);
            const double lastY = timeToY(m_chart.durationMilliseconds);
            painter.drawRect(QRectF(area.left() + lane * laneWidth, std::min(firstY, lastY), laneWidth,
                std::abs(lastY - firstY)));
        }
    }
    painter.setPen(QPen(palette::gridLine, 1.0));
    for (int lane = 1; lane < 6; ++lane) {
        const double x = area.left() + laneWidth * lane;
        painter.drawLine(QPointF(x, area.top()), QPointF(x, area.bottom()));
    }
    drawDividers(painter, area);

    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& note = m_chart.notes.at(index);
        if (note.kind != NoteKind::Tap && note.kind != NoteKind::Hold) {
            continue;
        }
        const qint64 endTime = note.kind == NoteKind::Hold ? note.endMilliseconds : note.startMilliseconds + kTapDurationMilliseconds;
        const double startY = timeToY(note.startMilliseconds);
        const double endY = timeToY(endTime);
        if (std::max(startY, endY) < area.top() || std::min(startY, endY) > area.bottom()) {
            continue;
        }
        const LaneSpan startSpan = laneSpan(note, note.startX, note.startWidth);
        const LaneSpan endSpan = laneSpan(note, note.endX, note.endWidth);
        const QPolygonF object{
            QPointF(area.left() + startSpan.left * laneWidth + 1.0, startY),
            QPointF(area.left() + startSpan.right * laneWidth - 1.0, startY),
            QPointF(area.left() + endSpan.right * laneWidth - 1.0, endY),
            QPointF(area.left() + endSpan.left * laneWidth + 1.0, endY),
        };
        const QColor color = groundColor(note, opacity);
        const QColor contour = palette::withAlpha(palette::floorNoteContour, opacity);
        painter.setPen(QPen(contour, 1.0));
        painter.setBrush(color);
        painter.drawPolygon(object);
        if (m_selectedHitObjectIndexes.contains(index)) {
            painter.setPen(QPen(palette::guideYellow, 2.0));
            painter.setBrush(Qt::NoBrush);
            painter.drawPolygon(object);
        }
    }
}

void FlatView::drawSky(QPainter& painter, const QRectF& area, const int opacity) const {
    const auto skyX = [&area](const double coordinate) {
        return area.left() + std::clamp(coordinate, 0.0, 1.0) * area.width();
    };

    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& note = m_chart.notes.at(index);
        if (note.kind != NoteKind::Sky) {
            continue;
        }

        const double startY = timeToY(note.startMilliseconds);
        const double endY = timeToY(note.endMilliseconds);
        if (std::max(startY, endY) < area.top() || std::min(startY, endY) > area.bottom()) {
            continue;
        }

        const qint64 duration = std::max(note.endMilliseconds - note.startMilliseconds, qint64(1));
        QPolygonF leftSide;
        QPolygonF rightSide;
        leftSide.reserve(kZoneSamples + 1);
        rightSide.reserve(kZoneSamples + 1);
        for (int sampleIndex = 0; sampleIndex <= kZoneSamples; ++sampleIndex) {
            const double progress = static_cast<double>(sampleIndex) / kZoneSamples;
            const double left = zoneLeftEdge(note, true)
                + (zoneLeftEdge(note, false) - zoneLeftEdge(note, true))
                    * easingFor(note.auxiliary, false, progress);
            const double right = zoneRightEdge(note, true)
                + (zoneRightEdge(note, false) - zoneRightEdge(note, true))
                    * easingFor(note.auxiliary, true, progress);
            const qint64 time = note.startMilliseconds + static_cast<qint64>(duration * progress);
            leftSide.append(QPointF(skyX(left), timeToY(time)));
            rightSide.append(QPointF(skyX(right), timeToY(time)));
        }
        QPolygonF zone = leftSide;
        for (auto iterator = rightSide.crbegin(); iterator != rightSide.crend(); ++iterator) {
            zone.append(*iterator);
        }
        const QColor fill = palette::withAlpha(palette::skyArea, std::min(opacity, 105));
        painter.setPen(QPen(palette::withAlpha(palette::skyArea.lighter(115), opacity), 1.5));
        painter.setBrush(fill);
        painter.drawPolygon(zone);
        if (shouldDrawZoneControls(index)) {
            drawZoneControls(painter, area, note);
        }
    }

    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& note = m_chart.notes.at(index);
        if (note.kind != NoteKind::Flick) {
            continue;
        }
        const double noteY = timeToY(note.startMilliseconds);
        const double triangleHeight = flickTriangleHeight();
        if (noteY + triangleHeight < area.top() || noteY - triangleHeight > area.bottom()) {
            continue;
        }
        const bool pointsRight = (note.auxiliary & kFlickRight) != 0 || (note.auxiliary & kFlickLeft) == 0;
        const double direction = pointsRight ? 1.0 : -1.0;
        const double width = std::clamp(
            note.startWidth > 0.0 ? note.startWidth : note.endWidth,
            kMinimumFlickTrailWidth,
            1.0);
        const double leadingCoordinate = note.startX + direction * width * 0.5;
        const QPointF leadingPoint(skyX(leadingCoordinate), noteY);
        const QPointF fullTrailingPoint(skyX(leadingCoordinate - direction * width), noteY);
        const double headLegLength = std::min(triangleHeight, std::abs(leadingPoint.x() - fullTrailingPoint.x()));
        const double triangleLength = headLegLength / area.width();
        const double trailingCoordinate = leadingCoordinate - direction * triangleLength;
        const QPointF trailingPoint(skyX(trailingCoordinate), noteY);
        const QPointF trailingCorner = trailingPoint + QPointF(0.0, -headLegLength);
        const QColor color = palette::withAlpha(pointsRight ? palette::guideGreen : palette::guideYellow, opacity);
        const QColor fillColor(color.red(), color.green(), color.blue(), std::min(opacity, 180));
        const double trailLength = std::max(0.0, width - triangleLength);
        QPolygonF trail;
        trail.reserve(kFlickTrailSamples + 3);
        for (int sampleIndex = 0; sampleIndex <= kFlickTrailSamples; ++sampleIndex) {
            const double progress = static_cast<double>(sampleIndex) / kFlickTrailSamples;
            const double coordinate = trailingCoordinate - direction * trailLength * progress;
            const QPointF baselinePoint(skyX(coordinate), noteY);
            trail.append(baselinePoint + QPointF(0.0, -headLegLength * (1.0 - std::sqrt(progress))));
        }
        trail.append(trailingPoint);
        painter.setPen(QPen(color, 1.5));
        painter.setBrush(fillColor);
        painter.drawPolygon(trail);
        painter.drawPolygon(QPolygonF{trailingPoint, leadingPoint, trailingCorner});
        if (shouldDrawFlickControls(index)) {
            drawFlickControls(painter, area, note);
        }
    }
}

void FlatView::invalidateSelectedZoneConnections() {
    m_selectedZoneConnectionsDirty = true;
}

void FlatView::ensureSelectedZoneConnections() const {
    if (!m_selectedZoneConnectionsDirty) {
        return;
    }
    m_selectedConnectedZoneIndexes.clear();
    m_selectedZoneConnectionsDirty = false;
    const auto sharesEndpoint = [](const ChartNote& first, const ChartNote& second) {
        constexpr qint64 kTimeTolerance = 1;
        constexpr double kEdgeToleranceValue = 0.0001;
        for (const bool firstAtStart : {true, false}) {
            for (const bool secondAtStart : {true, false}) {
                if (std::abs((firstAtStart ? first.startMilliseconds : first.endMilliseconds)
                        - (secondAtStart ? second.startMilliseconds : second.endMilliseconds)) <= kTimeTolerance
                    && std::abs(zoneLeftEdge(first, firstAtStart) - zoneLeftEdge(second, secondAtStart)) <= kEdgeToleranceValue
                    && std::abs(zoneRightEdge(first, firstAtStart) - zoneRightEdge(second, secondAtStart)) <= kEdgeToleranceValue) {
                    return true;
                }
            }
        }
        return false;
    };

    QVector<int> pending;
    for (const int selectedIndex : m_selectedHitObjects) {
        if (selectedIndex >= 0 && selectedIndex < m_chart.notes.size()
            && m_chart.notes.at(selectedIndex).kind == NoteKind::Sky) {
            m_selectedConnectedZoneIndexes.insert(selectedIndex);
            pending.append(selectedIndex);
        }
    }
    while (!pending.isEmpty()) {
        const int currentIndex = pending.takeLast();
        const ChartNote& current = m_chart.notes.at(currentIndex);
        for (int candidateIndex = 0; candidateIndex < m_chart.notes.size(); ++candidateIndex) {
            const ChartNote& candidate = m_chart.notes.at(candidateIndex);
            if (m_selectedConnectedZoneIndexes.contains(candidateIndex) || candidate.kind != NoteKind::Sky
                || candidate.groupId != current.groupId || !sharesEndpoint(current, candidate)) {
                continue;
            }
            m_selectedConnectedZoneIndexes.insert(candidateIndex);
            pending.append(candidateIndex);
        }
    }
}

bool FlatView::shouldDrawZoneControls(const int hitObjectIndex) const {
    if (hitObjectIndex < 0 || hitObjectIndex >= m_chart.notes.size()
        || m_chart.notes.at(hitObjectIndex).kind != NoteKind::Sky) {
        return false;
    }
    ensureSelectedZoneConnections();
    return m_selectedConnectedZoneIndexes.contains(hitObjectIndex);
}

bool FlatView::shouldDrawFlickControls(const int hitObjectIndex) const {
    if (hitObjectIndex < 0 || hitObjectIndex >= m_chart.notes.size()
        || m_chart.notes.at(hitObjectIndex).kind != NoteKind::Flick) {
        return false;
    }
    return m_selectedHitObjectIndexes.contains(hitObjectIndex);
}

void FlatView::drawFlickControls(QPainter& painter, const QRectF& area, const ChartNote& hitObject) const {
    const SkySpan span = flickSpan(hitObject);
    const double y = timeToY(hitObject.startMilliseconds);
    const std::array<QPointF, 2> controls{{
        QPointF(area.left() + span.left * area.width(), y),
        QPointF(area.left() + span.right * area.width(), y),
    }};

    painter.setPen(QPen(palette::white, 1.0));
    painter.setBrush(palette::floorNoteBlue);
    for (const QPointF& control : controls) {
        painter.drawRect(QRectF(control.x() - kZoneControlRadius, control.y() - kZoneControlRadius,
            kZoneControlRadius * 2.0, kZoneControlRadius * 2.0));
    }
}

void FlatView::drawZoneControls(QPainter& painter, const QRectF& area, const ChartNote& hitObject) const {
    const auto skyX = [&area](const double coordinate) {
        return area.left() + std::clamp(coordinate, 0.0, 1.0) * area.width();
    };
    const qint64 duration = std::max(hitObject.endMilliseconds - hitObject.startMilliseconds, qint64(1));
    const double middleProgress = 0.5;
    const double middleLeft = zoneLeftEdge(hitObject, true)
        + (zoneLeftEdge(hitObject, false) - zoneLeftEdge(hitObject, true))
            * easingFor(hitObject.auxiliary, false, middleProgress);
    const double middleRight = zoneRightEdge(hitObject, true)
        + (zoneRightEdge(hitObject, false) - zoneRightEdge(hitObject, true))
            * easingFor(hitObject.auxiliary, true, middleProgress);
    const qint64 middleTime = hitObject.startMilliseconds + duration / 2;
    const std::array<QPointF, 4> edgeControls{{
        QPointF(skyX(zoneLeftEdge(hitObject, true)), timeToY(hitObject.startMilliseconds)),
        QPointF(skyX(zoneRightEdge(hitObject, true)), timeToY(hitObject.startMilliseconds)),
        QPointF(skyX(zoneLeftEdge(hitObject, false)), timeToY(hitObject.endMilliseconds)),
        QPointF(skyX(zoneRightEdge(hitObject, false)), timeToY(hitObject.endMilliseconds)),
    }};
    const std::array<QPointF, 2> sideControls{{
        QPointF(skyX(middleLeft), timeToY(middleTime)),
        QPointF(skyX(middleRight), timeToY(middleTime)),
    }};

    painter.setPen(QPen(palette::white, 1.0));
    painter.setBrush(palette::skyArea);
    for (const QPointF& control : edgeControls) {
        painter.drawRect(QRectF(control.x() - kZoneControlRadius, control.y() - kZoneControlRadius,
            kZoneControlRadius * 2.0, kZoneControlRadius * 2.0));
    }
    painter.setBrush(palette::guideYellow);
    for (const QPointF& control : sideControls) {
        painter.drawEllipse(control, kZoneControlRadius, kZoneControlRadius);
    }
}

} // namespace infalsus::gui
