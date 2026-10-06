#include "gui/canvas/conveyor_view.h"

#include "gui/app/game_palette.h"
#include "gui/canvas/conveyor_geometry.h"

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

// Track uses a 3.333333334-second visual approach at 1.00x note speed. Its
// culling and perspective ranges are divided by the player's note-speed
// setting, independently of chart SV.
constexpr double kBaseApproachDistanceMilliseconds = 3333.333334;
constexpr qint64 kTapDepthMilliseconds = 30;
constexpr int kFlickTrailSamples = 18;
constexpr int kSkyCurveSamples = 18;
constexpr double kPi = 3.14159265358979323846;
constexpr double kSkyCoordinateMinimum = 0.0;
constexpr double kSkyCoordinateMaximum = 1.0;
constexpr std::uint32_t kCenterSineOut = 0x08U;
constexpr std::uint32_t kCenterSineIn = 0x10U;
constexpr std::uint32_t kWidthSineOut = 0x40U;
constexpr std::uint32_t kWidthSineIn = 0x80U;
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
// floorPoint/skyPoint sample the receiver's final 5% of perspective depth.
// Extending that tangent by 20 reproduces one full approach interval at the
// same local screen velocity, rather than making -1x look four times slower.
constexpr double kNegativePlaneExtension = 20.0;
constexpr double kFloorLaneWidth = 0.25;
constexpr double kFloorHalfLaneWidth = kFloorLaneWidth * 0.5;
constexpr double kCentralFloorLeftBoundary = 0.125;
constexpr double kCentralFloorRightBoundary = 1.125;

struct FloorSpan {
    double left = 0.0;
    double right = 0.0;
};

struct TimeRange {
    qint64 startMilliseconds = 0;
    qint64 endMilliseconds = 0;
};

[[nodiscard]] double zoneLeftEdge(const ChartNote& note, const bool atStart) {
    const double center = atStart ? note.startX : note.endX;
    const double width = atStart ? note.startWidth : note.endWidth;

    return center - width * 0.5;
}

[[nodiscard]] double zoneRightEdge(const ChartNote& note, const bool atStart) {
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

[[nodiscard]] QPointF extendedFloorPoint(const double coordinate, const double approach) {
    const QPointF receiver = ConveyorGeometry::floorPoint(coordinate, 1.0);
    const QPointF inside = ConveyorGeometry::floorPoint(coordinate, 0.95);
    return receiver + (receiver - inside) * ((1.0 - std::clamp(approach, 0.0, 1.0)) * kNegativePlaneExtension);
}

[[nodiscard]] QPointF extendedSkyPoint(const double coordinate, const double approach) {
    const QPointF receiver = ConveyorGeometry::skyPoint(coordinate, 1.0);
    const QPointF inside = ConveyorGeometry::skyPoint(coordinate, 0.95);
    return receiver + (receiver - inside) * ((1.0 - std::clamp(approach, 0.0, 1.0)) * kNegativePlaneExtension);
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
    m_playbackPositionMilliseconds = 0;
    update();
}

void ConveyorView::setTimingPoints(QVector<infalsus::TimingPoint> timingPoints) {
    std::stable_sort(timingPoints.begin(), timingPoints.end(),
        [](const infalsus::TimingPoint& first, const infalsus::TimingPoint& second) {
            return first.timeMilliseconds < second.timeMilliseconds;
        });
    m_timingPoints = std::move(timingPoints);
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
    update();
}

void ConveyorView::removeHitObject(const int index) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes.removeAt(index);
    update();
}

void ConveyorView::updateHitObject(const int index, const ChartNote& hitObject) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes[index] = hitObject;
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
        update();
    }
}

void ConveyorView::paintGL() {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette::canvasBackground);
    painter.save();
    painter.setTransform(painterTransform(rect()));
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

double ConveyorView::approachDistanceMilliseconds() const {
    return kBaseApproachDistanceMilliseconds / m_noteSpeed;
}

double ConveyorView::noteOpacity(const qint64 chartTimeMilliseconds) const {
    // Track_RenderLogicalNote feeds the material a cubic smoothstep based on
    // (1 - abs(signedCoordinateDelta / approachRange)) / 0.1.  Candidates at
    // the horizon therefore become transparent over the outermost 10% of
    // their permitted distance; drawing them fully opaque makes folded-SV
    // re-entries appear that are invisible in gameplay.
    const double fadeProgress = std::clamp(approachProgress(chartTimeMilliseconds) / 0.1, 0.0, 1.0);
    return (3.0 - 2.0 * fadeProgress) * fadeProgress * fadeProgress;
}

double ConveyorView::scrollCoordinateAt(const qint64 chartTimeMilliseconds) const {
    // Mirrors GameAssembly's Conveyor_IntegrateScrollCoordinate: each kind-0
    // speed becomes active after its timestamp and contributes a signed,
    // piecewise-linear interval to the chart's rendered coordinate.
    const qint64 target = std::max(chartTimeMilliseconds, qint64(0));
    qint64 cursor = 0;
    double speed = 1.0;
    double coordinate = 0.0;
    for (const infalsus::SpeedEvent& event : m_speedEvents) {
        if (event.timeMilliseconds < cursor) {
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

QPointF ConveyorView::floorPointAt(const double coordinate, const qint64 chartTimeMilliseconds) const {
    if (chartTimeMilliseconds <= m_playbackPositionMilliseconds) {
        return ConveyorGeometry::floorPoint(coordinate, 1.0);
    }
    const double delta = scrollDistanceTo(chartTimeMilliseconds);
    const double progress = approachProgress(chartTimeMilliseconds);
    return delta < 0.0
        ? extendedFloorPoint(coordinate, progress)
        : ConveyorGeometry::floorPoint(coordinate, progress);
}

QPointF ConveyorView::skyPointAt(const double coordinate, const qint64 chartTimeMilliseconds) const {
    if (chartTimeMilliseconds <= m_playbackPositionMilliseconds) {
        return ConveyorGeometry::skyPoint(coordinate, 1.0);
    }
    const double delta = scrollDistanceTo(chartTimeMilliseconds);
    const double progress = approachProgress(chartTimeMilliseconds);
    return delta < 0.0
        ? extendedSkyPoint(coordinate, progress)
        : ConveyorGeometry::skyPoint(coordinate, progress);
}

double ConveyorView::easingFor(const std::uint32_t auxiliary, const bool rightSide, const double progress) const {
    const std::uint32_t mode = rightSide ? (auxiliary & 0xE0U) : (auxiliary & 0x1CU);
    const double clampedProgress = std::clamp(progress, 0.0, 1.0);
    const std::uint32_t sineOut = rightSide ? kWidthSineOut : kCenterSineOut;
    const std::uint32_t sineIn = rightSide ? kWidthSineIn : kCenterSineIn;

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

    // The outer floor lanes carry their gameplay-side colour. Keeping the
    // central lanes neutral prevents the entire receiver from reading blue.
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::withAlpha(palette::outerNotePurple, 46));
    painter.drawPolygon(ConveyorGeometry::floorLane(0));
    painter.setBrush(palette::withAlpha(palette::outerNoteRed, 46));
    painter.drawPolygon(ConveyorGeometry::floorLane(kLaneCount - 1));

    // Render OFF->ON lane intervals as bounded floor zones. They approach the
    // receiver with their start/end timestamps just like Hold objects.
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::withAlpha(palette::canvasBackground.darker(180), 180));
    for (int lane = 0; lane < kLaneCount; ++lane) {
        bool dimming = false;
        qint64 zoneStart = 0;
        const auto drawZone = [this, &painter, lane](const qint64 start, const qint64 end) {
            if (!isVisible(start, end) || end <= start) {
                return;
            }
            const qint64 clippedStart = std::max(start, m_playbackPositionMilliseconds);
            const qint64 clippedEnd = end;
            const double left = -0.125 + static_cast<double>(lane) * 0.25;
            const double right = left + 0.25;
            painter.drawPolygon(QPolygonF{
                floorPointAt(left, clippedStart),
                floorPointAt(right, clippedStart),
                floorPointAt(right, clippedEnd),
                floorPointAt(left, clippedEnd),
            });
        };
        for (const infalsus::LaneEvent& event : m_laneEvents) {
            if (event.lane != lane) {
                continue;
            }
            if (!event.enabled && !dimming) {
                zoneStart = event.timeMilliseconds;
                dimming = true;
            } else if (event.enabled && dimming) {
                drawZone(zoneStart, event.timeMilliseconds);
                dimming = false;
            }
        }
        if (dimming) {
            drawZone(zoneStart, m_chart.durationMilliseconds);
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

void ConveyorView::drawDivisors(QPainter& painter) {
    const double approachDistance = approachDistanceMilliseconds();
    if (m_timingPoints.isEmpty()) {
        return;
    }

    // A signed SV coordinate is monotonic only between speed events. Find
    // the future chart-time ranges that lie inside the rendered approach
    // distance, then lay timing divisions into those ranges. This keeps a
    // negative section on the receptor side of the conveyor and avoids
    // iterating every division in a long chart each frame.
    const qint64 chartEnd = std::max(
        m_chart.durationMilliseconds,
        m_playbackPositionMilliseconds + static_cast<qint64>(std::ceil(approachDistance)));
    QVector<qint64> boundaries{m_playbackPositionMilliseconds, chartEnd};
    for (const infalsus::SpeedEvent& event : m_speedEvents) {
        if (event.timeMilliseconds > m_playbackPositionMilliseconds && event.timeMilliseconds < chartEnd) {
            boundaries.append(event.timeMilliseconds);
        }
    }
    std::sort(boundaries.begin(), boundaries.end());
    boundaries.erase(std::unique(boundaries.begin(), boundaries.end()), boundaries.end());

    QVector<TimeRange> visibleRanges;
    for (int index = 1; index < boundaries.size(); ++index) {
        const qint64 segmentStart = boundaries.at(index - 1);
        const qint64 segmentEnd = boundaries.at(index);
        if (segmentEnd <= segmentStart) {
            continue;
        }
        const double startCoordinate = scrollDistanceTo(segmentStart);
        const double coordinateChange = scrollDistanceTo(segmentEnd) - startCoordinate;
        if (std::abs(coordinateChange) < 0.000001) {
            if (std::abs(startCoordinate) <= approachDistance) {
                visibleRanges.append({segmentStart, segmentEnd});
            }
            continue;
        }

        const double firstCrossing = (-approachDistance - startCoordinate) / coordinateChange;
        const double secondCrossing = (approachDistance - startCoordinate) / coordinateChange;
        const double rangeStart = std::max(0.0, std::min(firstCrossing, secondCrossing));
        const double rangeEnd = std::min(1.0, std::max(firstCrossing, secondCrossing));
        if (rangeStart >= rangeEnd) {
            continue;
        }
        const qint64 start = segmentStart + static_cast<qint64>(std::ceil(
            static_cast<double>(segmentEnd - segmentStart) * rangeStart));
        const qint64 end = segmentStart + static_cast<qint64>(std::ceil(
            static_cast<double>(segmentEnd - segmentStart) * rangeEnd));
        if (end > start) {
            visibleRanges.append({start, end});
        }
    }

    for (int pointIndex = 0; pointIndex < m_timingPoints.size(); ++pointIndex) {
        const infalsus::TimingPoint& point = m_timingPoints.at(pointIndex);
        const qint64 sectionStart = point.timeMilliseconds;
        const qint64 sectionEnd = pointIndex + 1 < m_timingPoints.size()
            ? m_timingPoints.at(pointIndex + 1).timeMilliseconds
            : chartEnd;
        const double beatDuration = 60000.0 / point.beatsPerMinute;
        const double measureDuration = beatDuration * point.timeSignatureNumerator * 4.0
            / std::max(point.timeSignatureDenominator, 1);
        if (measureDuration <= 0.0 || sectionEnd <= sectionStart) {
            continue;
        }

        for (const TimeRange range : visibleRanges) {
            const qint64 rangeStart = std::max(range.startMilliseconds, sectionStart);
            const qint64 rangeEnd = std::min(range.endMilliseconds, sectionEnd);
            if (rangeEnd <= rangeStart) {
                continue;
            }
            const qint64 firstMeasure = std::max<qint64>(0, static_cast<qint64>(std::floor(
                static_cast<double>(rangeStart - sectionStart) / measureDuration)));
            for (qint64 measureIndex = firstMeasure;; ++measureIndex) {
                const double measureStart = static_cast<double>(sectionStart) + measureIndex * measureDuration;
                if (measureStart >= rangeEnd || measureStart >= sectionEnd) {
                    break;
                }
                const qint64 timeMilliseconds = static_cast<qint64>(std::llround(measureStart));
                if (timeMilliseconds < rangeStart || timeMilliseconds >= rangeEnd
                    || timeMilliseconds < sectionStart || timeMilliseconds >= sectionEnd) {
                    continue;
                }

                painter.setPen(QPen(palette::withAlpha(palette::white, 220), 2.0));
                // The game draws three ground-only divider strips. The outer
                // lanes have their own angled strips while lanes 1–4 share
                // the straight central strip.
                painter.drawLine(
                    floorPointAt(-0.125, timeMilliseconds),
                    floorPointAt(0.125, timeMilliseconds));
                painter.drawLine(
                    floorPointAt(0.125, timeMilliseconds),
                    floorPointAt(1.125, timeMilliseconds));
                painter.drawLine(
                    floorPointAt(1.125, timeMilliseconds),
                    floorPointAt(1.375, timeMilliseconds));
            }
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
    const QPolygonF shape{
        floorPointAt(startSpan.left, noteStart),
        floorPointAt(startSpan.right, noteStart),
        floorPointAt(endSpan.right, rawEnd),
        floorPointAt(endSpan.left, rawEnd),
    };
    const QColor color = floorColor(note);

    painter.save();
    painter.setOpacity(painter.opacity() * noteOpacity(noteStart));
    const QColor contour = palette::floorNoteContour;
    painter.setPen(QPen(contour, kNoteContourWidth));
    painter.setBrush(color);
    painter.drawPolygon(shape);
    painter.restore();
}

void ConveyorView::drawFlick(QPainter& painter, const ChartNote& note) {
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

void ConveyorView::drawSkyZone(QPainter& painter, const ChartNote& note) {
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
        const double left = zoneLeftEdge(note, true)
            + (zoneLeftEdge(note, false) - zoneLeftEdge(note, true))
                * easingFor(note.auxiliary, false, segmentProgress);
        const double right = zoneRightEdge(note, true)
            + (zoneRightEdge(note, false) - zoneRightEdge(note, true))
                * easingFor(note.auxiliary, true, segmentProgress);
        const qint64 chartTime = static_cast<qint64>(sampleTime);
        leftBoundary.append(skyPointAt(left, chartTime));
        rightBoundary.append(skyPointAt(right, chartTime));
    }

    QPolygonF zone = leftBoundary;
    for (auto iterator = rightBoundary.crbegin(); iterator != rightBoundary.crend(); ++iterator) {
        zone.append(*iterator);
    }

    const QColor color = palette::withAlpha(palette::skyArea, kSkyFillAlpha);
    painter.save();
    painter.setOpacity(painter.opacity() * noteOpacity(clippedStart));
    painter.setPen(QPen(palette::skyArea.lighter(115), kSkyContourWidth));
    painter.setBrush(color);
    painter.drawPolygon(zone);
    painter.restore();
}

void ConveyorView::drawChart(QPainter& painter) {
    // Track_BuildVisibleNoteCandidates works independently on the four raw
    // side buffers.  Its coordinate window is signed: a note is accepted only
    // when its start/end span touches [current - range, current + range].
    // It deliberately keeps walking through a skipped note, only stopping a
    // side after its start is beyond current + 2 * range.  A global time-order
    // pass cannot reproduce that behaviour across central, outer, and sky
    // buffers when a negative-SV section folds their coordinates.
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
            return first->startMilliseconds < second->startMilliseconds;
        });

        for (const ChartNote* note : sideNotes) {
            if (note->endMilliseconds <= m_playbackPositionMilliseconds) {
                continue;
            }

            // Native code keeps objects whose start timestamp is less than a
            // second ahead of the playhead.  This bypasses its coordinate
            // test, so retain the same short receiver-side grace interval.
            if (note->startMilliseconds - m_playbackPositionMilliseconds < 1000) {
                visibleNotes.append(note);
                continue;
            }

            const double startDistance = scrollDistanceTo(note->startMilliseconds);
            if (startDistance > 2.0 * approachDistance) {
                break;
            }

            const double endDistance = scrollDistanceTo(note->endMilliseconds);
            if (startDistance > approachDistance
                || endDistance < -approachDistance) {
                continue;
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
            drawSkyZone(painter, *note);
        }
    }

    for (const ChartNote* note : visibleNotes) {
        if (note->kind == NoteKind::Flick) {
            drawFlick(painter, *note);
        }
    }
}
