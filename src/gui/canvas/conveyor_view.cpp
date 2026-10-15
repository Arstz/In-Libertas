#include "gui/canvas/conveyor_view.h"

#include "gui/app/game_palette.h"
#include "gui/canvas/conveyor_geometry.h"
#include "core/field_groups.h"

#include <QtCore/QLineF>
#include <QtCore/QRectF>
#include <QtGui/QBrush>
#include <QtGui/QFontMetrics>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <QtGui/QTransform>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

namespace palette = infalsus::gui::palette;

constexpr double kBaseApproachDistanceMilliseconds = 3333.333334;
constexpr qint64 kTapDepthMilliseconds = 30;
constexpr int kFlickTrailSamples = 18;
constexpr int kSkyCurveSamples = 18;
constexpr double kPi = 3.14159265358979323846;
constexpr double kSkyCoordinateMinimum = 0.0;
constexpr double kSkyCoordinateMaximum = 1.0;
constexpr std::uint32_t kLeftSineOut = 0x08U;
constexpr std::uint32_t kLeftSineIn = 0x10U;
constexpr std::uint32_t kRightSineOut = 0x40U;
constexpr std::uint32_t kRightSineIn = 0x80U;
constexpr std::uint32_t kFlickRight = 0x400U;
constexpr std::uint32_t kFlickLeft = 0x1000U;
constexpr double kFlickTriangleHeight = 90.0;
constexpr double kMinimumFlickTrailWidth = 0.06;
constexpr int kLaneCount = 6;
constexpr int kBackgroundAlpha = 120;
constexpr int kSkyFillAlpha = 105;
constexpr qreal kRailWidth = 4.0;
constexpr qreal kSkyContourWidth = 3.0;
constexpr qreal kNoteContourWidth = 2.0;
constexpr double kFloorLaneWidth = 0.25;
constexpr double kFloorHalfLaneWidth = kFloorLaneWidth * 0.5;
constexpr double kCentralFloorLeftBoundary = 0.125;
constexpr double kCentralFloorRightBoundary = 1.125;

struct FloorSpan {
    double left = 0.0;
    double right = 0.0;
};

[[nodiscard]] double fieldLeftEdge(const ChartNote& note, const bool atStart) {
    const double center = atStart ? note.startX : note.endX;
    const double width = atStart ? note.startWidth : note.endWidth;

    return center - width * 0.5;
}

[[nodiscard]] double fieldRightEdge(const ChartNote& note, const bool atStart) {
    const double center = atStart ? note.startX : note.endX;
    const double width = atStart ? note.startWidth : note.endWidth;

    return center + width * 0.5;
}

[[nodiscard]] QTransform painterTransform(const QRectF& viewport) {
    const QSizeF canvasSize = ConveyorGeometry::nativeSize();
    const double scale = std::min(viewport.width() / canvasSize.width(), viewport.height() / canvasSize.height());
    const QSizeF scaledSize = canvasSize * scale;
    const double offsetX = viewport.left() + (viewport.width() - scaledSize.width()) * 0.5;
    const double offsetY = viewport.top() + (viewport.height() - scaledSize.height()) * 0.5;

    QTransform transform;
    transform.translate(offsetX, offsetY);
    transform.scale(scale, scale);

    return transform;
}

[[nodiscard]] FloorSpan floorSpan(const FloorSide side, const double firstLaneCenter, const double width) {
    switch (side) {
    case FloorSide::LeftOuter:
        return {.left = -kFloorHalfLaneWidth, .right = kFloorHalfLaneWidth};
    case FloorSide::RightOuter:
        return {.left = 1.25 - kFloorHalfLaneWidth, .right = 1.25 + kFloorHalfLaneWidth};
    case FloorSide::Central: {
        const double left = std::clamp(
            firstLaneCenter - kFloorHalfLaneWidth,
            kCentralFloorLeftBoundary,
            kCentralFloorRightBoundary - kFloorLaneWidth);
        const double spanWidth = std::clamp(
            width,
            kFloorLaneWidth,
            kCentralFloorRightBoundary - left);

        return {.left = left, .right = left + spanWidth};
    }
    default:
        return {.left = firstLaneCenter - kFloorHalfLaneWidth, .right = firstLaneCenter + kFloorHalfLaneWidth};
    }
}

[[nodiscard]] QColor floorColor(const ChartNote& note) {
    if (note.side == FloorSide::LeftOuter) {
        return palette::outerNotePurple;
    }
    if (note.side == FloorSide::RightOuter) {
        return palette::outerNoteRed;
    }
    if (note.kind == NoteKind::Hold) {
        return palette::floorNoteBlue;
    }
    if (std::max(note.startWidth, note.endWidth) <= kFloorLaneWidth) {
        return palette::floorNoteBlue;
    }

    return palette::white;
}

[[nodiscard]] QColor difficultyColor(const infalsus::Difficulty difficulty) {
    switch (difficulty) {
    case infalsus::Difficulty::Minimal:
        return QColor(0, 137, 170);
    case infalsus::Difficulty::Evolved:
        return QColor(0, 190, 180);
    case infalsus::Difficulty::Ultimate:
        return QColor(132, 73, 204);
    case infalsus::Difficulty::Forbidden:
        return QColor(209, 43, 78);
    }

    return palette::floorNoteBlue;
}

[[nodiscard]] QString difficultyCardLabel(const infalsus::Difficulty difficulty) {
    switch (difficulty) {
    case infalsus::Difficulty::Minimal:
        return QStringLiteral("MIN");
    case infalsus::Difficulty::Evolved:
        return QStringLiteral("EVO");
    case infalsus::Difficulty::Ultimate:
        return QStringLiteral("ULT");
    case infalsus::Difficulty::Forbidden:
        return QStringLiteral("FBD");
    }

    return {};
}

} // namespace

ConveyorView::ConveyorView(QWidget* parent)
    : QOpenGLWidget(parent) {
    setMinimumSize(400, 225);
    setAutoFillBackground(false);
}

void ConveyorView::setChart(const ChartData& chart) {
    m_chart = chart;
    m_fieldBoundariesDirty = true;
    m_measureMarkersDirty = true;
    m_playbackPositionMilliseconds = 0;
    update();
}

void ConveyorView::setTimingPoints(QVector<infalsus::TimingPoint> timingPoints) {
    std::stable_sort(timingPoints.begin(), timingPoints.end(),
        [](const infalsus::TimingPoint& first, const infalsus::TimingPoint& second) {
            return first.timeMilliseconds < second.timeMilliseconds;
        });
    m_timingPoints = std::move(timingPoints);
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::setLaneEvents(QVector<infalsus::LaneEvent> laneEvents) {
    std::stable_sort(laneEvents.begin(), laneEvents.end(), [](const infalsus::LaneEvent& first, const infalsus::LaneEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_laneEvents = std::move(laneEvents);
    update();
}

void ConveyorView::setSpeedEvents(QVector<infalsus::SpeedEvent> speedEvents) {
    std::stable_sort(speedEvents.begin(), speedEvents.end(), [](const infalsus::SpeedEvent& first, const infalsus::SpeedEvent& second) {
        return first.timeMilliseconds < second.timeMilliseconds;
    });
    m_speedEvents = std::move(speedEvents);
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::setSongCard(const infalsus::ChartMetadata& metadata, const infalsus::Difficulty difficulty,
    const infalsus::DifficultyMetadata difficultyMetadata, QImage jacket) {
    m_cardMetadata = metadata;
    m_cardDifficulty = difficulty;
    m_cardDifficultyMetadata = difficultyMetadata;
    m_cardJacket = std::move(jacket);
    update();
}

void ConveyorView::setNoteSpeed(const double noteSpeed) {
    const double normalizedSpeed = std::clamp(noteSpeed, 0.1, 10.0);
    if (qFuzzyCompare(m_noteSpeed, normalizedSpeed)) {
        return;
    }
    m_noteSpeed = normalizedSpeed;
    update();
}

void ConveyorView::setPlaybackPosition(const qint64 positionMilliseconds) {
    if (m_playbackPositionMilliseconds == positionMilliseconds) {
        return;
    }
    m_playbackPositionMilliseconds = positionMilliseconds;
    update();
}

void ConveyorView::addHitObject(const int index, const ChartNote& hitObject) {
    const int insertionIndex = std::clamp(index, 0, static_cast<int>(m_chart.notes.size()));
    m_chart.notes.insert(insertionIndex, hitObject);
    m_fieldBoundariesDirty = true;
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::removeHitObject(const int index) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes.removeAt(index);
    m_fieldBoundariesDirty = true;
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::updateHitObject(const int index, const ChartNote& hitObject) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes[index] = hitObject;
    m_fieldBoundariesDirty = true;
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::updateHitObjects(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects) {
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
        m_fieldBoundariesDirty = true;
        m_measureMarkersDirty = true;
        update();
    }
}

void ConveyorView::paintGL() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette::canvasBackground);
    painter.save();
    painter.setTransform(painterTransform(rect()));
    painter.setClipRect(QRectF(QPointF(0.0, 0.0), ConveyorGeometry::nativeSize()));
    drawConveyor(painter);
    drawChart(painter);
    painter.restore();
    drawSongCard(painter);
}

void ConveyorView::drawSongCard(QPainter& painter) const {
    if (m_cardMetadata.songName.trimmed().isEmpty() && m_cardJacket.isNull()) {
        return;
    }

    // The reference layout is the 1400x900 editor workspace.  Retaining that
    // ratio keeps the card visually tied to the 3D viewport rather than to a
    // fixed pixel size when the user resizes a dock.
    const double scale = std::clamp(std::min(width() / 1400.0, height() / 900.0), 0.65, 1.75);
    const double margin = 12.0 * scale;
    const double cardHeight = 128.0 * scale;
    const double cardWidth = 548.0 * scale;
    const double jacketInset = 7.0 * scale;
    const QRectF available = rect().adjusted(margin, margin, -margin, -margin);
    const QRectF card(available.right() - std::min(cardWidth, available.width()), available.top(),
        std::min(cardWidth, available.width()), std::min(cardHeight, available.height()));
    if (card.width() < 120.0 || card.height() < 70.0) {
        return;
    }

    const QColor accent = difficultyColor(m_cardDifficulty);
    const double jacketSize = std::min(card.height() - jacketInset * 2.0, 114.0 * scale);
    const QRectF jacketRect(card.left() + jacketInset, card.top() + jacketInset, jacketSize, jacketSize);
    const double contentLeft = jacketRect.right() + 17.0 * scale;
    const QRectF detailsRect(contentLeft, card.top() + 11.0 * scale,
        std::max(0.0, card.right() - contentLeft - 16.0 * scale), card.height() - 22.0 * scale);

    painter.save();
    painter.setPen(QPen(QColor(95, 138, 151, 190), 1.0));
    painter.setBrush(QColor(7, 25, 34, 246));
    painter.drawRect(card);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 170));
    painter.drawRect(QRectF(card.left(), card.top(), 6.0 * scale, card.height()));
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 56));
    painter.drawRect(QRectF(jacketRect.left() - jacketInset, card.top(), jacketRect.width() + jacketInset * 2.0,
        card.height()));

    const double lowerBandTop = card.bottom() - 27.0 * scale;
    painter.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 64));
    painter.drawRect(QRectF(card.left() + 1.0, lowerBandTop, card.width() - 2.0, card.bottom() - lowerBandTop - 1.0));
    painter.setPen(QPen(QColor(92, 131, 143, 130), 1.0));
    painter.drawLine(QPointF(card.left() + 1.0, lowerBandTop), QPointF(card.right() - 1.0, lowerBandTop));

    if (m_cardJacket.isNull()) {
        painter.setPen(QPen(QColor(125, 158, 167), 1.0));
        painter.setBrush(QColor(20, 43, 53));
        painter.drawRect(jacketRect);
        painter.setPen(QColor(159, 188, 195));
        painter.drawText(jacketRect, Qt::AlignCenter, QStringLiteral("NO\nJACKET"));
    } else {
        painter.drawImage(jacketRect, m_cardJacket);
        painter.setPen(QPen(QColor(203, 236, 241, 205), 1.0));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(jacketRect);
    }

    QFont titleFont = painter.font();
    titleFont.setPointSizeF(24.0 * scale);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(QColor(240, 250, 252));
    const QFontMetrics titleMetrics(titleFont);
    painter.drawText(QRectF(detailsRect.left(), detailsRect.top(), detailsRect.width(), titleMetrics.height() + 4.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        titleMetrics.elidedText(m_cardMetadata.songName, Qt::ElideRight, static_cast<int>(detailsRect.width())));

    QFont artistFont = titleFont;
    artistFont.setPointSizeF(16.8 * scale);
    artistFont.setWeight(QFont::Normal);
    painter.setFont(artistFont);
    painter.setPen(QColor(202, 217, 221));
    const QFontMetrics artistMetrics(artistFont);
    painter.drawText(QRectF(detailsRect.left(), detailsRect.top() + titleMetrics.height() + 6.0,
            detailsRect.width(), artistMetrics.height() + 3.0),
        Qt::AlignLeft | Qt::AlignVCenter,
        artistMetrics.elidedText(m_cardMetadata.artistName, Qt::ElideRight, static_cast<int>(detailsRect.width())));

    QFont difficultyFont = artistFont;
    difficultyFont.setPointSizeF(13.8 * scale);
    difficultyFont.setWeight(QFont::DemiBold);
    painter.setFont(difficultyFont);
    painter.setPen(QColor(226, 245, 247));
    const QRectF difficultyRect(contentLeft, lowerBandTop + 2.0 * scale, detailsRect.width(), 23.0 * scale);
    painter.drawText(difficultyRect, Qt::AlignLeft | Qt::AlignVCenter,
        QStringLiteral("%1    %2").arg(difficultyCardLabel(m_cardDifficulty)).arg(m_cardDifficultyMetadata.rating));
    painter.restore();
}

double ConveyorView::approachProgress(const qint64 chartTimeMilliseconds) const {
    const double coordinateDelta = scrollDistanceTo(chartTimeMilliseconds);
    const double progress = 1.0 - std::abs(coordinateDelta) / approachDistanceMilliseconds();

    return std::clamp(progress, 0.0, 1.0);
}

double ConveyorView::projectionProgress(const qint64 chartTimeMilliseconds) const {
    return 1.0 - scrollDistanceTo(chartTimeMilliseconds) / approachDistanceMilliseconds();
}

double ConveyorView::approachDistanceMilliseconds() const {
    return kBaseApproachDistanceMilliseconds / m_noteSpeed;
}

double ConveyorView::noteOpacity(const qint64 chartTimeMilliseconds) const {
    const double fadeProgress = std::clamp(approachProgress(chartTimeMilliseconds) / 0.1, 0.0, 1.0);
    return (3.0 - 2.0 * fadeProgress) * fadeProgress * fadeProgress;
}

double ConveyorView::scrollCoordinateAt(const double chartTimeMilliseconds) const {
    const double target = chartTimeMilliseconds;
    qint64 cursor = 0;
    double speed = 1.0;
    double coordinate = 0.0;
    if (target < 0) {
        for (const infalsus::SpeedEvent& event : m_speedEvents) {
            if (event.timeMilliseconds > 0) {
                break;
            }
            if (event.timeMilliseconds == 0 && std::isfinite(event.speed)) {
                speed = event.speed;
            }
        }

        return static_cast<double>(target) * speed;
    }
    for (const infalsus::SpeedEvent& event : m_speedEvents) {
        if (event.timeMilliseconds < cursor || !std::isfinite(event.speed)) {
            continue;
        }
        if (event.timeMilliseconds > target) {
            break;
        }
        coordinate += static_cast<double>(event.timeMilliseconds - cursor) * speed;
        cursor = event.timeMilliseconds;
        speed = event.speed;
    }
    return coordinate + static_cast<double>(target - cursor) * speed;
}

double ConveyorView::scrollDistanceTo(const qint64 chartTimeMilliseconds) const {
    return scrollCoordinateAt(chartTimeMilliseconds)
        - scrollCoordinateAt(m_playbackPositionMilliseconds);
}

bool ConveyorView::isVisible(const qint64 startMilliseconds, const qint64 endMilliseconds) const {
    if (endMilliseconds <= m_playbackPositionMilliseconds) {
        return false;
    }
    const double approachDistance = approachDistanceMilliseconds();
    QVector<qint64> breakpoints{std::max(startMilliseconds, m_playbackPositionMilliseconds), endMilliseconds};
    for (const infalsus::SpeedEvent& event : m_speedEvents) {
        if (event.timeMilliseconds > breakpoints.first() && event.timeMilliseconds < endMilliseconds) {
            breakpoints.append(event.timeMilliseconds);
        }
    }
    std::sort(breakpoints.begin(), breakpoints.end());
    breakpoints.erase(std::unique(breakpoints.begin(), breakpoints.end()), breakpoints.end());

    if (breakpoints.size() == 1) {
        return std::abs(scrollDistanceTo(breakpoints.constFirst())) <= approachDistance;
    }

    for (int index = 1; index < breakpoints.size(); ++index) {
        const double first = scrollDistanceTo(breakpoints.at(index - 1));
        const double second = scrollDistanceTo(breakpoints.at(index));
        if (std::min(first, second) <= approachDistance && std::max(first, second) >= -approachDistance) {
            return true;
        }
    }
    return false;
}

QPointF ConveyorView::skyPointAt(const double coordinate, const qint64 chartTimeMilliseconds) const {
    return ConveyorGeometry::skyPoint(coordinate, projectionProgress(chartTimeMilliseconds));
}

double ConveyorView::easingFor(const std::uint32_t auxiliary, const bool rightSide, const double progress) const {
    const std::uint32_t mode = rightSide ? (auxiliary & 0xE0U) : (auxiliary & 0x1CU);
    const double clampedProgress = std::clamp(progress, 0.0, 1.0);
    const std::uint32_t sineOut = rightSide ? kRightSineOut : kLeftSineOut;
    const std::uint32_t sineIn = rightSide ? kRightSineIn : kLeftSineIn;

    if (mode == sineOut) {
        return std::sin(clampedProgress * kPi * 0.5);
    }
    if (mode == sineIn) {
        return 1.0 - std::cos(clampedProgress * kPi * 0.5);
    }

    return clampedProgress;
}

void ConveyorView::drawConveyor(QPainter& painter) {
    const QPolygonF floor = ConveyorGeometry::floorOutline();
    QLinearGradient gradient(QPointF(1280.0, 127.0), QPointF(1280.0, 1288.0));
    gradient.setColorAt(0.0, palette::panelHover);
    gradient.setColorAt(1.0, palette::chrome);
    painter.setPen(QPen(palette::canvasBackground.darker(180), kRailWidth));
    painter.setBrush(QBrush(gradient));
    painter.drawPolygon(floor);

    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::withAlpha(palette::outerNotePurple, 46));
    painter.drawPath(ConveyorGeometry::floorLane(0));
    painter.setBrush(palette::withAlpha(palette::outerNoteRed, 46));
    painter.drawPath(ConveyorGeometry::floorLane(kLaneCount - 1));

    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::withAlpha(palette::canvasBackground.darker(180), 180));
    for (int lane = 0; lane < kLaneCount; ++lane) {
        bool dimming = false;
        qint64 intervalStart = 0;
        const auto drawInterval = [this, &painter, lane](const qint64 start, const qint64 end) {
            if (!isVisible(start, end) || end <= start) {
                return;
            }
            const qint64 clippedStart = std::max(start, m_playbackPositionMilliseconds);
            const qint64 clippedEnd = end;
            painter.drawPath(ConveyorGeometry::floorLane(lane,
                projectionProgress(clippedStart), projectionProgress(clippedEnd)));
        };
        for (const infalsus::LaneEvent& event : m_laneEvents) {
            if (event.lane != lane) {
                continue;
            }
            if (!event.enabled && !dimming) {
                intervalStart = event.timeMilliseconds;
                dimming = true;
            } else if (event.enabled && dimming) {
                drawInterval(intervalStart, event.timeMilliseconds);
                dimming = false;
            }
        }
        if (dimming) {
            drawInterval(intervalStart, m_chart.durationMilliseconds);
        }
    }

    drawDivisors(painter);

    const QPen railPen(palette::canvasBackground.darker(180), kRailWidth);
    painter.setPen(railPen);
    painter.setBrush(Qt::NoBrush);
    for (int laneIndex = 0; laneIndex <= kLaneCount; ++laneIndex) {
        const double boundary = -0.125 + static_cast<double>(laneIndex) * 0.25;
        painter.drawLine(ConveyorGeometry::floorPoint(boundary, 0.0), ConveyorGeometry::floorPoint(boundary, 1.0));
    }

    const QPolygonF floorReceiver = ConveyorGeometry::floorReceiver();
    painter.setPen(QPen(palette::outerNotePurple, kRailWidth));
    painter.drawLine(floorReceiver.at(0), floorReceiver.at(1));
    painter.setPen(QPen(palette::floorNoteBlue, kRailWidth));
    painter.drawPolyline(floorReceiver.mid(1, kLaneCount - 1));
    painter.setPen(QPen(palette::outerNoteRed, kRailWidth));
    painter.drawLine(floorReceiver.at(kLaneCount - 1), floorReceiver.at(kLaneCount));

    const QLineF skyReceiver = ConveyorGeometry::skyReceiver();
    painter.setPen(QPen(palette::floorNoteBlue, kRailWidth));
    painter.drawLine(skyReceiver);
    painter.setPen(QPen(palette::withAlpha(palette::floorNoteBlue, kBackgroundAlpha), 2.0));
    painter.drawLine(ConveyorGeometry::skyHorizon());
}

void ConveyorView::ensureMeasureMarkers() {
    if (!m_measureMarkersDirty) {
        return;
    }
    QVector<infalsus::TimingPoint> timingPoints = m_timingPoints;
    if (timingPoints.isEmpty()) {
        timingPoints.append(infalsus::TimingPoint{});
    }
    if (timingPoints.front().timeMilliseconds > 0) {
        infalsus::TimingPoint initial = timingPoints.front();
        initial.timeMilliseconds = 0;
        timingPoints.prepend(initial);
    }
    m_measureMarkers.clear();
    const double initialBpm = timingPoints.front().beatsPerMinute;
    const double initialMeasureDuration = 240000.0 / (std::isfinite(initialBpm) && initialBpm > 0.0
        ? initialBpm : infalsus::TimingPoint{}.beatsPerMinute);
    double chartEnd = m_chart.durationMilliseconds;
    for (const ChartNote& note : m_chart.notes) {
        chartEnd = std::max(chartEnd, static_cast<double>(note.endMilliseconds));
    }
    chartEnd += initialMeasureDuration * 10.0;
    const auto speedBefore = [this](const double time) {
        double speed = 1.0;
        for (const infalsus::SpeedEvent& event : m_speedEvents) {
            if (event.timeMilliseconds >= time && event.timeMilliseconds > 0) {
                break;
            }
            if (event.timeMilliseconds >= 0 && std::isfinite(event.speed)) {
                speed = event.speed;
            }
        }

        return speed;
    };
    if (std::isfinite(initialMeasureDuration) && initialMeasureDuration > 0.0 && speedBefore(0.0) >= 0.0) {
        for (int measureIndex = -9; measureIndex <= 0; ++measureIndex) {
            const double time = initialMeasureDuration * measureIndex;
            m_measureMarkers.append({time, scrollCoordinateAt(time)});
        }
    }
    for (int pointIndex = 0; pointIndex < timingPoints.size(); ++pointIndex) {
        const infalsus::TimingPoint& point = timingPoints.at(pointIndex);
        const double measureDuration = 60000.0 / point.beatsPerMinute * point.timeSignatureNumerator;
        const double sectionEnd = pointIndex + 1 < timingPoints.size()
            ? timingPoints.at(pointIndex + 1).timeMilliseconds : chartEnd;
        if (!std::isfinite(measureDuration) || measureDuration <= 0.0) {
            continue;
        }
        for (qint64 measureIndex = 0;; ++measureIndex) {
            const double time = point.timeMilliseconds + measureIndex * measureDuration;
            if (time >= sectionEnd) {
                break;
            }
            if (speedBefore(time) >= 0.0) {
                m_measureMarkers.append({time, scrollCoordinateAt(time)});
            }
        }
    }
    m_measureMarkersDirty = false;
}

void ConveyorView::drawDivisors(QPainter& painter) {
    ensureMeasureMarkers();
    const double currentCoordinate = scrollCoordinateAt(m_playbackPositionMilliseconds);
    const double approachDistance = approachDistanceMilliseconds();
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::withAlpha(palette::white, 220));
    for (const MeasureMarker& marker : m_measureMarkers) {
        if (marker.timeMilliseconds <= m_playbackPositionMilliseconds) {
            continue;
        }
        const double distance = (marker.coordinate - currentCoordinate) / approachDistance;
        if (std::abs(distance) <= 1.0) {
            painter.drawPath(ConveyorGeometry::measureMarker(distance));
        }
    }
}

void ConveyorView::drawFloorNote(QPainter& painter, const ChartNote& note) {
    const qint64 rawEnd = note.kind == NoteKind::Hold ? note.endMilliseconds : note.startMilliseconds + kTapDepthMilliseconds;
    if (rawEnd <= m_playbackPositionMilliseconds) {
        return;
    }
    const qint64 noteStart = std::max(note.startMilliseconds, m_playbackPositionMilliseconds);
    const FloorSpan startSpan = floorSpan(note.side, note.startX, note.startWidth);
    const FloorSpan endSpan = floorSpan(note.side, note.endX, note.endWidth);
    const QPainterPath shape = ConveyorGeometry::floorPath({
        {startSpan.left, projectionProgress(noteStart)}, {startSpan.right, projectionProgress(noteStart)},
        {endSpan.right, projectionProgress(rawEnd)}, {endSpan.left, projectionProgress(rawEnd)}}, note.side);
    const QColor color = floorColor(note);

    painter.save();
    painter.setOpacity(painter.opacity() * noteOpacity(noteStart));
    const QColor contour = palette::floorNoteContour;
    painter.setPen(QPen(contour, kNoteContourWidth));
    painter.setBrush(color);
    painter.drawPath(shape);
    painter.restore();
}

void ConveyorView::drawFlick(QPainter& painter, const ChartNote& note) {
    if (!ConveyorGeometry::skyVisible(projectionProgress(note.startMilliseconds))) {
        return;
    }
    const auto pointAt = [this, &note](const double coordinate) {
        return skyPointAt(coordinate, note.startMilliseconds);
    };
    const bool pointsRight = (note.auxiliary & kFlickRight) != 0 || (note.auxiliary & kFlickLeft) == 0;
    const double direction = pointsRight ? 1.0 : -1.0;
    const double trailWidth = std::clamp(
        note.startWidth > 0.0 ? note.startWidth : note.endWidth,
        kMinimumFlickTrailWidth,
        kSkyCoordinateMaximum);
    const double leadingCoordinate = note.startX + direction * trailWidth * 0.5;
    const QPointF leadingPoint = pointAt(leadingCoordinate);
    const double receiverSkyWidth = std::abs(
        ConveyorGeometry::skyPoint(kSkyCoordinateMaximum, 1.0).x()
        - ConveyorGeometry::skyPoint(kSkyCoordinateMinimum, 1.0).x());
    const double receiverTrailWidth = receiverSkyWidth * trailWidth;
    const double originalHeadLegLength = std::min(kFlickTriangleHeight, receiverTrailWidth);
    const double fullSkyWidth = std::abs(
        pointAt(kSkyCoordinateMaximum).x()
        - pointAt(kSkyCoordinateMinimum).x());
    const double perspectiveScale = receiverSkyWidth > 0.0 ? fullSkyWidth / receiverSkyWidth : 0.0;
    const double headLegLength = originalHeadLegLength * perspectiveScale;
    const double triangleLength = receiverSkyWidth > 0.0 ? originalHeadLegLength / receiverSkyWidth : 0.0;
    const double trailingCoordinate = leadingCoordinate - direction * triangleLength;
    const QPointF trailingPoint = pointAt(trailingCoordinate);
    const QPointF rightAngle = trailingPoint;
    const QPointF tip = leadingPoint;
    const QPointF trailingCorner = trailingPoint + QPointF(0.0, -headLegLength);
    const QPolygonF triangle{rightAngle, tip, trailingCorner};
    const QColor color = pointsRight ? palette::guideGreen : palette::guideYellow;
    const QColor fillColor(color.red(), color.green(), color.blue(), 180);
    const double trailLength = std::max(0.0, trailWidth - triangleLength);
    QPolygonF trail;
    trail.reserve(kFlickTrailSamples + 3);

    for (int sampleIndex = 0; sampleIndex <= kFlickTrailSamples; ++sampleIndex) {
        const double progress = static_cast<double>(sampleIndex) / static_cast<double>(kFlickTrailSamples);
        const double coordinate = trailingCoordinate - direction * trailLength * progress;
        const QPointF baselinePoint = pointAt(coordinate);
        const double height = -headLegLength * (1.0 - std::sqrt(progress));
        const QPointF trailPoint = baselinePoint + QPointF(0.0, height);
        trail.append(trailPoint);
    }
    trail.append(rightAngle);

    painter.save();
    painter.setOpacity(painter.opacity() * noteOpacity(note.startMilliseconds));
    painter.setPen(QPen(color, kSkyContourWidth));
    painter.setBrush(fillColor);
    painter.drawPolygon(trail);
    painter.setBrush(fillColor);
    painter.drawPolygon(triangle);
    painter.restore();
}

void ConveyorView::ensureFieldBoundaries() {
    if (!m_fieldBoundariesDirty) {
        return;
    }
    m_sharedFieldStarts.clear();
    m_sharedFieldEnds.clear();
    for (const infalsus::FieldGroup& group : infalsus::analyzeFieldGroups(m_chart)) {
        for (const infalsus::FieldJoint& joint : group.joints) {
            (joint.firstAtStart ? m_sharedFieldStarts : m_sharedFieldEnds)[joint.firstIndex].append(joint.segment);
            (joint.secondAtStart ? m_sharedFieldStarts : m_sharedFieldEnds)[joint.secondIndex].append(joint.segment);
        }
    }
    for (QVector<infalsus::FieldSegment>& segments : m_sharedFieldStarts) {
        segments = infalsus::mergeFieldSegments(std::move(segments));
    }
    for (QVector<infalsus::FieldSegment>& segments : m_sharedFieldEnds) {
        segments = infalsus::mergeFieldSegments(std::move(segments));
    }
    m_fieldBoundariesDirty = false;
}

void ConveyorView::drawFieldBoundary(QPainter& painter, const double left, const double right,
    const qint64 timeMilliseconds, const QVector<infalsus::FieldSegment>& sharedSegments) {
    const QColor color = palette::skyArea.lighter(115);
    double cursor = left;

    painter.setPen(QPen(color, kSkyContourWidth));
    for (const infalsus::FieldSegment& segment : sharedSegments) {
        const double sharedLeft = std::clamp(segment.left, left, right);
        const double sharedRight = std::clamp(segment.right, left, right);
        if (sharedLeft > cursor) {
            painter.drawPath(ConveyorGeometry::skyPath({{cursor, projectionProgress(timeMilliseconds)},
                {sharedLeft, projectionProgress(timeMilliseconds)}}, false));
        }
        cursor = std::max(cursor, sharedRight);
    }
    if (cursor < right) {
        painter.drawPath(ConveyorGeometry::skyPath({{cursor, projectionProgress(timeMilliseconds)},
            {right, projectionProgress(timeMilliseconds)}}, false));
    }
}

void ConveyorView::drawSkyField(QPainter& painter, const ChartNote& note, const int index) {
    if (note.endMilliseconds <= m_playbackPositionMilliseconds) {
        return;
    }
    const qint64 clippedStart = std::max(note.startMilliseconds, m_playbackPositionMilliseconds);
    const qint64 clippedEnd = note.endMilliseconds;
    const qint64 segmentDuration = std::max(note.endMilliseconds - note.startMilliseconds, qint64(1));
    QPolygonF leftBoundary;
    QPolygonF rightBoundary;
    leftBoundary.reserve(kSkyCurveSamples + 1);
    rightBoundary.reserve(kSkyCurveSamples + 1);

    for (int sampleIndex = 0; sampleIndex <= kSkyCurveSamples; ++sampleIndex) {
        const double sampleProgress = static_cast<double>(sampleIndex) / static_cast<double>(kSkyCurveSamples);
        const double sampleTime = static_cast<double>(clippedStart)
            + static_cast<double>(clippedEnd - clippedStart) * sampleProgress;
        const double segmentProgress = (sampleTime - note.startMilliseconds) / static_cast<double>(segmentDuration);
        const double left = fieldLeftEdge(note, true)
            + (fieldLeftEdge(note, false) - fieldLeftEdge(note, true))
                * easingFor(note.auxiliary, false, segmentProgress);
        const double right = fieldRightEdge(note, true)
            + (fieldRightEdge(note, false) - fieldRightEdge(note, true))
                * easingFor(note.auxiliary, true, segmentProgress);
        const qint64 chartTime = static_cast<qint64>(sampleTime);
        leftBoundary.append({left, projectionProgress(chartTime)});
        rightBoundary.append({right, projectionProgress(chartTime)});
    }

    QPolygonF field = leftBoundary;
    for (auto iterator = rightBoundary.crbegin(); iterator != rightBoundary.crend(); ++iterator) {
        field.append(*iterator);
    }

    const QColor color = palette::withAlpha(palette::skyArea, kSkyFillAlpha);
    const QColor contourColor = palette::skyArea.lighter(115);
    const bool originalStartVisible = note.startMilliseconds > m_playbackPositionMilliseconds;
    painter.save();
    painter.setOpacity(painter.opacity() * noteOpacity(clippedStart));
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawPath(ConveyorGeometry::skyPath(field));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(contourColor, kSkyContourWidth));
    painter.drawPath(ConveyorGeometry::skyPath(leftBoundary, false));
    painter.drawPath(ConveyorGeometry::skyPath(rightBoundary, false));
    drawFieldBoundary(painter, fieldLeftEdge(note, false), fieldRightEdge(note, false), note.endMilliseconds,
        m_sharedFieldEnds.value(index));
    if (originalStartVisible) {
        drawFieldBoundary(painter, fieldLeftEdge(note, true), fieldRightEdge(note, true), note.startMilliseconds,
            m_sharedFieldStarts.value(index));
    } else {
        painter.setPen(QPen(contourColor, kSkyContourWidth));
        painter.drawPath(ConveyorGeometry::skyPath({leftBoundary.front(), rightBoundary.front()}, false));
    }
    painter.restore();
}

void ConveyorView::drawChart(QPainter& painter) {
    ensureFieldBoundaries();
    std::array<QVector<const ChartNote*>, 4> notesBySide;
    for (const ChartNote& note : m_chart.notes) {
        const int sideIndex = static_cast<int>(note.side);
        if (sideIndex >= 0 && sideIndex < static_cast<int>(notesBySide.size())) {
            notesBySide.at(static_cast<std::size_t>(sideIndex)).append(&note);
        }
    }

    QVector<const ChartNote*> visibleNotes;
    visibleNotes.reserve(m_chart.notes.size());
    const double approachDistance = approachDistanceMilliseconds();
    for (QVector<const ChartNote*>& sideNotes : notesBySide) {
        std::stable_sort(sideNotes.begin(), sideNotes.end(), [](const ChartNote* first, const ChartNote* second) {
            if (first->startMilliseconds != second->startMilliseconds) {
                return first->startMilliseconds < second->startMilliseconds;
            }

            return first->endMilliseconds < second->endMilliseconds;
        });

        for (const ChartNote* note : sideNotes) {
            const bool completed = note->endMilliseconds <= m_playbackPositionMilliseconds;
            if (completed && (note->kind != NoteKind::Sky
                    || m_playbackPositionMilliseconds - note->endMilliseconds > 1000)) {
                continue;
            }

            if (note->startMilliseconds - m_playbackPositionMilliseconds < 1000) {
                visibleNotes.append(note);
                continue;
            }

            const double startDistance = scrollDistanceTo(note->startMilliseconds);
            if (startDistance > 2.0 * approachDistance) {
                break;
            }

            visibleNotes.append(note);
        }
    }

    for (const ChartNote* note : visibleNotes) {
        if (note->kind == NoteKind::Tap || note->kind == NoteKind::Hold) {
            drawFloorNote(painter, *note);
        }
    }

    for (const ChartNote* note : visibleNotes) {
        if (note->kind == NoteKind::Sky) {
            drawSkyField(painter, *note, static_cast<int>(note - m_chart.notes.constData()));
        }
    }

    for (const ChartNote* note : visibleNotes) {
        if (note->kind == NoteKind::Flick) {
            drawFlick(painter, *note);
        }
    }
}
