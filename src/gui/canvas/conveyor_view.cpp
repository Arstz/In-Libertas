#include "gui/canvas/conveyor_view.h"

#include "gui/app/game_palette.h"
#include "gui/app/playtest_settings.h"
#include "gui/canvas/conveyor_geometry.h"
#include "gui/key_bindings.h"
#include "core/field_groups.h"

#include <QtCore/QLineF>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>
#include <QtCore/QRectF>
#include <QtCore/QSet>
#include <QtGui/QBrush>
#include <QtGui/QFontDatabase>
#include <QtGui/QFontMetrics>
#include <QtGui/QLinearGradient>
#include <QtGui/QPaintEvent>
#include <QtGui/QPainter>
#include <QtGui/QPainterPathStroker>
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
constexpr int kComboGlyphPixelSize = 256;
constexpr double kComboEmWorldSize = 0.39 * 1.3 * 0.9;
constexpr double kComboStrokeWidth = 0.39 * 1.23 / 16.0 * 0.5 * 0.9;
constexpr double kJudgementLifetimeMilliseconds = 450.0;

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

[[nodiscard]] double overlayCardScale(const QSizeF& viewport) {
    return std::clamp(std::min(viewport.width() / 1400.0, viewport.height() / 900.0), 0.65, 1.75);
}

[[nodiscard]] QRectF overlayCardRect(const QRectF& viewport, const bool left, const bool paired) {
    const double scale = overlayCardScale(viewport.size());
    const double margin = 12.0 * scale;
    const QRectF available = viewport.adjusted(margin, margin, -margin, -margin);
    const double maximumWidth = paired ? (available.width() - margin) * 0.5 : available.width();
    const double width = std::max(0.0, std::min(548.0 * scale, maximumWidth));
    return {left ? available.left() : available.right() - width, available.top(),
        width, std::max(0.0, std::min(128.0 * scale, available.height()))};
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

[[nodiscard]] QPainterPath centeredGlyphs(const QString& text, QFont font, const double emSize,
    const QFont::Weight weight = QFont::DemiBold) {
    font.setPixelSize(kComboGlyphPixelSize);
    font.setWeight(weight);
    QPainterPath glyphs;
    glyphs.addText(QPointF(), font, text);
    const QPointF center = glyphs.boundingRect().center();
    QTransform transform;
    transform.scale(emSize / kComboGlyphPixelSize, emSize / kComboGlyphPixelSize);
    transform.translate(-center.x(), -center.y());
    return transform.map(glyphs);
}

[[nodiscard]] QFont gameplayNumberFont() {
    static const QFont font = [] {
        const QString path = QCoreApplication::applicationDirPath() + QStringLiteral("/assets/ultraproxi sb.otf");
        const int id = QFontDatabase::addApplicationFont(path);
        const QStringList families = QFontDatabase::applicationFontFamilies(id);
        if (families.isEmpty()) {
            qWarning() << "Could not load gameplay number font:" << path;
            return QFontDatabase::systemFont(QFontDatabase::FixedFont);
        }
        const QString family = families.first();
        return QFontDatabase::font(family, QFontDatabase::styles(family).value(0), 12);
    }();
    return font;
}

struct NumericAtlas {
    QImage image;
    QRectF bounds;
    double advance = 0.0;
    std::array<QRectF, 11> sources;
};

[[nodiscard]] NumericAtlas makeNumericAtlas(const bool outlined) {
    QFont font = gameplayNumberFont();
    font.setPixelSize(kComboGlyphPixelSize);
    font.setKerning(false);
    const QFontMetricsF metrics(font);
    NumericAtlas atlas;
    for (const QChar digit : QStringLiteral("0123456789")) {
        atlas.advance = std::max(atlas.advance, metrics.horizontalAdvance(digit));
    }
    const QString characters = QStringLiteral("0123456789'");
    const double baseline = metrics.capHeight() * 0.5;
    const double strokeWidth = outlined ? kComboStrokeWidth * kComboGlyphPixelSize / kComboEmWorldSize : 0.0;
    std::array<QPainterPath, 11> glyphs;
    QRectF inkBounds;
    for (int index = 0; index < characters.size(); ++index) {
        const QChar digit = characters.at(index);
        const double x = (atlas.advance - metrics.horizontalAdvance(digit)) * 0.5;
        glyphs[index].addText(QPointF(x, baseline), font, QString(digit));
        inkBounds = inkBounds.united(glyphs[index].boundingRect());
    }
    const double padding = std::ceil(strokeWidth * 0.5) + 2.0;
    atlas.bounds = inkBounds.united(QRectF(0.0, inkBounds.top(), atlas.advance, inkBounds.height()))
        .adjusted(-padding, -padding, padding, padding).toAlignedRect();
    const int cellWidth = qRound(atlas.bounds.width());
    const int cellHeight = qRound(atlas.bounds.height());
    atlas.image = QImage(cellWidth * 4, cellHeight * 3, QImage::Format_ARGB32_Premultiplied);
    atlas.image.fill(Qt::transparent);
    QPainter painter(&atlas.image);
    painter.setRenderHint(QPainter::Antialiasing);
    QPainterPathStroker stroker;
    stroker.setWidth(strokeWidth);
    stroker.setCapStyle(Qt::RoundCap);
    stroker.setJoinStyle(Qt::RoundJoin);
    for (int index = 0; index < characters.size(); ++index) {
        const QPointF origin((index % 4) * cellWidth, (index / 4) * cellHeight);
        atlas.sources[index] = QRectF(origin, QSizeF(cellWidth, cellHeight));
        painter.save();
        painter.translate(origin - atlas.bounds.topLeft());
        painter.setPen(Qt::NoPen);
        painter.setBrush(outlined ? QColor::fromRgbF(0.4823529, 0.8550977, 0.8588235, 0.2352941)
            : QColor(153, 247, 248));
        painter.drawPath(glyphs[index]);
        if (outlined) {
            painter.setBrush(QColor::fromRgbF(0.7905393, 0.8962264, 0.8863183, 0.4901961));
            painter.drawPath(stroker.createStroke(glyphs[index]));
        }
        painter.restore();
    }
    return atlas;
}

[[nodiscard]] const NumericAtlas& numericAtlas(const bool outlined) {
    static const NumericAtlas combo = makeNumericAtlas(true);
    static const NumericAtlas score = makeNumericAtlas(false);
    return outlined ? combo : score;
}

void drawFixedWidthNumber(QPainter& painter, const QString& text, const NumericAtlas& atlas, const double emSize) {
    const double scale = emSize / kComboGlyphPixelSize;
    const double left = -atlas.advance * text.size() * 0.5;
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (qsizetype index = 0; index < text.size(); ++index) {
        const QChar character = text.at(index);
        const int digit = character == QLatin1Char('\'') ? 10 : character.digitValue();
        if (digit < 0 || digit > 10) {
            continue;
        }
        const QRectF cell = atlas.bounds.translated(left + index * atlas.advance, 0.0);
        const QRectF target(cell.left() * scale, cell.top() * scale, cell.width() * scale, cell.height() * scale);
        painter.drawImage(target, atlas.image, atlas.sources[digit]);
    }
    painter.restore();
}

[[nodiscard]] std::array<QImage, 7> makeJudgementImages(QFont font) {
    static const QString labels[]{QString(), QStringLiteral("MISS"), QStringLiteral("NEAR EARLY"),
        QStringLiteral("NEAR LATE"), QStringLiteral("EXACT EARLY"), QStringLiteral("EXACT LATE"),
        QStringLiteral("EXACT+")};
    font.setPixelSize(68);
    font.setBold(true);
    std::array<QImage, 7> images;
    for (int index = 1; index < images.size(); ++index) {
        QPainterPath glyphs;
        glyphs.addText(QPointF(), font, labels[index]);
        const QRect bounds = glyphs.boundingRect().adjusted(-2.0, -2.0, 2.0, 2.0).toAlignedRect();
        QImage& image = images[index];
        image = QImage(bounds.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.translate(-bounds.topLeft());
        painter.fillPath(glyphs, palette::white);
    }
    return images;
}

} // namespace

ConveyorView::ConveyorView(QWidget* parent)
    : QOpenGLWidget(parent) {
    setMinimumSize(400, 225);
    setAutoFillBackground(false);
    setPlaytestLaneKeys(infalsus::gui::kDefaultPlaytestLaneKeys);
}

void ConveyorView::setPlaytestLaneKeys(const std::array<Qt::Key, 6>& keys) {
    for (std::size_t lane = 0; lane < keys.size(); ++lane) {
        const QString label = keys[lane] == Qt::Key_unknown ? QString()
            : QKeySequence(keys[lane]).toString(QKeySequence::NativeText);
        QPainterPath glyphs = centeredGlyphs(label, font(), 0.35, QFont::Bold);
        const double scale = std::min(1.0, 0.95 / std::max(0.95, glyphs.boundingRect().width()));
        QTransform transform;
        transform.translate(0.0, -0.32);
        transform.scale(scale, scale);
        m_keyIndicatorGlyphs[lane] = transform.map(glyphs);
    }
    update();
}

void ConveyorView::setChart(const ChartData& chart) {
    m_chart = chart;
    m_noteIndexesDirty = true;
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
    m_scrollPoints = {{0, 0.0, 1.0}};
    for (const auto& event : m_speedEvents) {
        if (event.timeMilliseconds < 0 || !std::isfinite(event.speed)) {
            continue;
        }
        ScrollPoint& previous = m_scrollPoints.last();
        const double coordinate = previous.coordinate + (event.timeMilliseconds - previous.time) * previous.speed;
        if (event.timeMilliseconds == previous.time) {
            previous.speed = event.speed;
        } else {
            m_scrollPoints.append({event.timeMilliseconds, coordinate, event.speed});
        }
    }
    m_noteIndexesDirty = true;
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

void ConveyorView::setPlaytestFrame(const infalsus::PlaytestSession& session, const QString& status,
    const QVector<infalsus::PlaytestJudgement>& judgements) {
    if (!m_playtest) {
        (void)numericAtlas(true);
        (void)numericAtlas(false);
        if (m_judgementImages[1].isNull()) {
            m_judgementImages = makeJudgementImages(font());
        }
    }
    m_playtest = true;
    QSet<std::uint64_t> heldFieldGroups;
    QSet<std::uint64_t> dueFieldGroups;
    for (const auto& state : session.notes()) {
        if (state.note.kind != NoteKind::Sky) {
            continue;
        }
        if (state.active) {
            heldFieldGroups.insert(state.note.groupId);
        }
        if (!state.consumed && state.note.startMilliseconds <= session.time()
            && state.note.endMilliseconds >= session.time()) {
            dueFieldGroups.insert(state.note.groupId);
        }
    }
    for (const auto& state : session.notes()) {
        bool active = state.note.kind == NoteKind::Sky
            ? heldFieldGroups.contains(state.note.groupId) : state.active;
        const bool holdTail = state.note.kind == NoteKind::Hold && state.consumed
            && session.time() < state.note.endMilliseconds;
        if (holdTail && session.time() >= state.note.startMilliseconds) {
            for (int lane = state.firstLane; lane <= state.lastLane; ++lane) {
                active = active || session.heldLanes()[lane];
            }
        }
        const bool missing = state.note.kind == NoteKind::Sky && !active
            && dueFieldGroups.contains(state.note.groupId);
        m_playtestNotes.insert(state.note.id, (state.consumed && !state.missed && !holdTail ? 1 : 0)
            | (active ? 2 : 0) | (missing ? 4 : 0));
        if (state.note.kind == NoteKind::Hold || state.note.kind == NoteKind::Sky) {
            const qint64 now = static_cast<qint64>(std::floor(session.time()));
            qint64 start = m_playtestNoteStarts.value(state.note.id, state.note.startMilliseconds);
            start = std::max(start, std::min(now, static_cast<qint64>(std::floor(state.successfulUntil))));
            if (active) {
                start = std::max(start, now);
            }
            m_playtestNoteStarts.insert(state.note.id, std::min(start, state.note.endMilliseconds));
        }
    }
    if (session.result().combo >= 2
        && (m_playtestResult.combo != session.result().combo || m_comboText.isEmpty())) {
        m_comboText = QString::number(session.result().combo);
    }
    if (m_scoreText.isEmpty() || m_playtestResult.score != session.result().score) {
        m_scoreText = QString::number(std::max<qint64>(0, session.result().score)).rightJustified(9, QLatin1Char('0'));
        for (int index = static_cast<int>(m_scoreText.size()) - 3; index > 0; index -= 3) {
            m_scoreText.insert(index, QStringLiteral("'"));
        }
    }
    m_playtestResult = session.result();
    m_playtestHeld = session.heldLanes();
    m_playtestCursor = session.cursor();
    m_sweepLeft = session.sweepLeft();
    m_sweepRight = session.sweepRight();
    m_playtestStatus = status;
    m_playbackPositionMilliseconds = static_cast<qint64>(std::floor(session.time()));
    m_judgements.erase(std::remove_if(m_judgements.begin(), m_judgements.end(), [&session](const auto& judgement) {
        return session.time() - judgement.time >= kJudgementLifetimeMilliseconds;
    }), m_judgements.end());
    for (const auto& judgement : judgements) {
        if (judgement.grade != infalsus::PlaytestGrade::Pending
            && session.time() - judgement.time < kJudgementLifetimeMilliseconds) {
            m_judgements.append(judgement);
        }
    }
    update();
}

void ConveyorView::endPlaytest() {
    m_playtest = false;
    m_playtestNotes.clear();
    m_playtestNoteStarts.clear();
    m_comboText.clear();
    m_scoreText.clear();
    m_judgements.clear();
    update();
}

double ConveyorView::gameplayMouseWidth() const {
    const QTransform transform = painterTransform(rect());
    const QLineF receiver = ConveyorGeometry::skyReceiver();
    return std::max(1.0, std::abs(transform.map(receiver.p2()).x() - transform.map(receiver.p1()).x()) * devicePixelRatioF());
}

void ConveyorView::addHitObject(const int index, const ChartNote& hitObject) {
    const int insertionIndex = std::clamp(index, 0, static_cast<int>(m_chart.notes.size()));
    m_chart.notes.insert(insertionIndex, hitObject);
    m_noteIndexesDirty = true;
    m_fieldBoundariesDirty = true;
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::removeHitObject(const int index) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes.removeAt(index);
    m_noteIndexesDirty = true;
    m_fieldBoundariesDirty = true;
    m_measureMarkersDirty = true;
    update();
}

void ConveyorView::updateHitObject(const int index, const ChartNote& hitObject) {
    if (index < 0 || index >= m_chart.notes.size()) {
        return;
    }

    m_chart.notes[index] = hitObject;
    m_noteIndexesDirty = true;
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
        m_noteIndexesDirty = true;
        m_fieldBoundariesDirty = true;
        m_measureMarkersDirty = true;
        update();
    }
}

void ConveyorView::paintGL() {
    m_frameScrollCoordinate = scrollCoordinateAt(m_playbackPositionMilliseconds);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.fillRect(rect(), palette::canvasBackground);
    painter.save();
    painter.setTransform(painterTransform(rect()));
    painter.setClipRect(QRectF(QPointF(0.0, 0.0), ConveyorGeometry::nativeSize()));
    drawConveyor(painter);
    drawChart(painter);
    if (m_playtest) {
        drawPlaytestCombo(painter);
        drawPlaytestInput(painter);
        drawPlaytestJudgements(painter);
    }
    painter.restore();
    drawSongCard(painter);
    if (m_playtest) {
        drawScoreCard(painter);
        drawPlaytestOverlay(painter);
    }
    drawControlHints(painter);
}

void ConveyorView::drawPlaytestCombo(QPainter& painter) const {
    if (m_playtestResult.combo < 2) {
        return;
    }
    painter.save();
    painter.setTransform(ConveyorGeometry::comboTextProjection(), true);
    drawFixedWidthNumber(painter, m_comboText, numericAtlas(true), kComboEmWorldSize);
    painter.restore();
}

void ConveyorView::drawPlaytestInput(QPainter& painter) const {
    const auto receiver = ConveyorGeometry::floorReceiver();
    std::array<bool, kLaneCount> enabled;
    enabled.fill(true);
    const qint64 laneEventCutoff = m_playbackPositionMilliseconds >= 50 ? m_playbackPositionMilliseconds : 5;
    for (const infalsus::LaneEvent& event : m_laneEvents) {
        if (event.timeMilliseconds >= laneEventCutoff) {
            break;
        }
        if (event.lane >= 0 && event.lane < kLaneCount) {
            enabled[event.lane] = event.enabled;
        }
    }
    for (int lane = 0; lane < kLaneCount;) {
        if (!m_playtestHeld[lane]) {
            ++lane;
            continue;
        }
        QPainterPath indicator;
        indicator.moveTo(receiver[lane]);
        QPainterPath clip;
        do {
            indicator.lineTo(receiver[lane + 1]);
            clip = clip.united(ConveyorGeometry::floorLane(lane, 1.01, 0.99));
            ++lane;
        } while (lane < kLaneCount && m_playtestHeld[lane]);
        painter.save();
        painter.setClipPath(clip, Qt::IntersectClip);
        painter.setPen(QPen(palette::white, 16.0, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(indicator);
        painter.restore();
    }
    for (int lane = 0; lane < kLaneCount; ++lane) {
        painter.save();
        painter.setTransform(ConveyorGeometry::keyIndicatorProjection(lane), true);
        painter.setOpacity(enabled[lane] ? 1.0 : 0.3);
        const bool pressed = m_playtestHeld[lane];
        QPainterPath keyShape;
        keyShape.addPolygon(QPolygonF{{-0.9, -12.0}, {0.9, -12.0}, {0.9, -0.65},
            {0.65, 0.0}, {-0.65, 0.0}, {-0.9, -0.65}});
        keyShape.closeSubpath();
        QLinearGradient fill(QPointF(0.0, 0.0), QPointF(0.0, -12.0));
        fill.setColorAt(0.0, QColor(151, 253, 253, pressed ? 130 : 65));
        fill.setColorAt(0.35, QColor(125, 235, 235, pressed ? 50 : 22));
        fill.setColorAt(1.0, QColor(125, 235, 235, 0));
        QLinearGradient outline(QPointF(0.0, 0.0), QPointF(0.0, -12.0));
        outline.setColorAt(0.0, QColor(175, 255, 255, pressed ? 240 : 140));
        outline.setColorAt(0.35, QColor(151, 253, 253, pressed ? 65 : 30));
        outline.setColorAt(1.0, QColor(151, 253, 253, 0));
        painter.setPen(QPen(QBrush(outline), pressed ? 0.025 : 0.015,
            Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        painter.setBrush(fill);
        painter.drawPath(keyShape);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor::fromRgbF(1.2691348791122437, 2.3396224975585938,
            2.3396224975585938, 0.3607843220233917));
        painter.drawPath(m_keyIndicatorGlyphs[lane]);
        painter.restore();
    }
    painter.setPen(QPen(QColor(188, 117, 255, 100), 8.0));
    painter.drawLine(ConveyorGeometry::skyPoint(m_sweepLeft, 1.0), ConveyorGeometry::skyPoint(m_sweepRight, 1.0));
    const QPointF cursor = ConveyorGeometry::skyPoint(m_playtestCursor, 1.0);
    const QPointF horizon = ConveyorGeometry::skyPoint(m_playtestCursor, 0.0);
    QLinearGradient guide(cursor, horizon);
    guide.setColorAt(0.0, QColor(255, 250, 124, 245));
    guide.setColorAt(0.65, QColor(244, 246, 81, 150));
    guide.setColorAt(1.0, QColor(244, 246, 81, 0));
    painter.setPen(QPen(QBrush(guide), 2.0));
    painter.drawLine(cursor, horizon);

    const QPointF tip = ConveyorGeometry::skyPoint(m_playtestCursor, 0.991);
    const QPointF left = ConveyorGeometry::skyPoint(m_playtestCursor - 0.055, 0.998);
    const QPointF right = ConveyorGeometry::skyPoint(m_playtestCursor + 0.055, 0.998);
    const QPointF middle = ConveyorGeometry::skyPoint(m_playtestCursor, 0.998);
    QLinearGradient triangle(tip, middle);
    triangle.setColorAt(0.0, QColor(231, 190, 255));
    triangle.setColorAt(0.25, QColor(172, 79, 239));
    triangle.setColorAt(1.0, QColor(172, 79, 239));
    painter.setPen(Qt::NoPen);
    painter.setBrush(triangle);
    painter.drawPolygon(QPolygonF{left, tip, right});
}

void ConveyorView::drawPlaytestJudgements(QPainter& painter) const {
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const auto& judgement : m_judgements) {
        const double age = m_playbackPositionMilliseconds - judgement.time;
        if (age < 0.0 || age >= kJudgementLifetimeMilliseconds) {
            continue;
        }
        const double fadeIn = 1.0 - std::pow(1.0 - std::clamp(age / 100.0, 0.0, 1.0), 3.0);
        const double fadeOut = 1.0 - std::clamp((age - 250.0) / 200.0, 0.0, 1.0);
        const double stretch = 1.0 - 0.15 * std::pow(1.0 - std::clamp(age / 200.0, 0.0, 1.0), 3.0);
        const QImage& image = m_judgementImages[static_cast<int>(judgement.grade)];
        const QPointF center = ConveyorGeometry::judgementTextPosition(judgement.position);
        const QSizeF size(image.width() * 0.5, image.height() * 0.5 * stretch);
        painter.setOpacity(fadeIn * fadeOut);
        painter.drawImage(QRectF(center - QPointF(size.width(), size.height()) * 0.5, size), image);
    }
    painter.restore();
}

void ConveyorView::drawPlaytestOverlay(QPainter& painter) const {
    painter.save();
    QFont font = painter.font();
    font.setPixelSize(std::clamp(height() / 34, 16, 34));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(palette::white);
    if (!m_playtestStatus.isEmpty()) {
        font.setPixelSize(std::clamp(height() / 18, 24, 72));
        painter.setFont(font);
        painter.drawText(rect(), Qt::AlignCenter, m_playtestStatus);
    }
    painter.restore();
}

void ConveyorView::drawScoreCard(QPainter& painter) const {
    const double scale = overlayCardScale(size());
    const QRectF card = overlayCardRect(rect(), true, true);
    if (card.width() < 120.0 || card.height() < 70.0) {
        return;
    }
    painter.save();
    painter.setPen(QPen(QColor(133, 200, 216, 200), 1.5 * scale));
    painter.setBrush(QColor(7, 25, 34, 246));
    painter.drawRect(card);
    painter.setPen(QPen(QColor(177, 245, 250, 220), 3.0 * scale));
    painter.drawLine(QPointF(card.left() + 7.0 * scale, card.top() + 8.0 * scale),
        QPointF(card.left() + 7.0 * scale, card.top() + 29.0 * scale));

    const QRectF content = card.adjusted(22.0 * scale, 9.0 * scale, -22.0 * scale, -9.0 * scale);
    QFont labelFont = painter.font();
    labelFont.setPointSizeF(12.0 * scale);
    labelFont.setWeight(QFont::DemiBold);
    painter.setFont(labelFont);
    painter.setPen(QColor(186, 215, 225));
    painter.drawText(QRectF(content.left(), content.top(), content.width(), 23.0 * scale),
        Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("SCORE"));

    const double emSize = 68.0 * scale;
    const NumericAtlas& atlas = numericAtlas(false);
    const double layoutWidth = atlas.advance * m_scoreText.size() * emSize / kComboGlyphPixelSize;
    const double fit = std::min(1.0, content.width() / std::max(1.0, layoutWidth));
    const QRectF valueRect(content.left(), content.top() + 25.0 * scale, content.width(),
        content.height() - 25.0 * scale);
    painter.translate(valueRect.left() + layoutWidth * fit * 0.5, valueRect.center().y());
    painter.scale(fit, fit);
    drawFixedWidthNumber(painter, m_scoreText, atlas, emSize);
    painter.restore();
}

void ConveyorView::drawControlHints(QPainter& painter) const {
    const QString text = m_playtest ? QStringLiteral("Esc: Editor")
        : QStringLiteral("F5: Playtest from current time");
    painter.save();
    QFont hintFont = painter.font();
    hintFont.setPointSizeF(14.0);
    hintFont.setWeight(QFont::Normal);
    painter.setFont(hintFont);
    painter.setPen(QColor(150, 150, 150));
    const QFontMetricsF metrics(hintFont);
    const QStringList lines = text.split(QLatin1Char('\n'));
    double blockWidth = 0.0;
    for (const QString& line : lines) {
        blockWidth = std::max(blockWidth, metrics.horizontalAdvance(line));
    }
    const double margin = 12.0 * overlayCardScale(size());
    const double blockHeight = metrics.lineSpacing() * lines.size();
    const QRectF block(margin, height() - margin - blockHeight,
        std::max(0.0, std::min(blockWidth + 2.0, width() - 2.0 * margin)), blockHeight);
    painter.drawText(block, Qt::AlignLeft | Qt::AlignBottom, text);
    painter.restore();
}

void ConveyorView::drawSongCard(QPainter& painter) const {
    if (m_cardMetadata.songName.trimmed().isEmpty() && m_cardJacket.isNull()) {
        return;
    }

    const double scale = overlayCardScale(size());
    const double jacketInset = 7.0 * scale;
    const QRectF card = overlayCardRect(rect(), false, m_playtest);
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
    const double progress = m_playtest ? projectionProgress(chartTimeMilliseconds) : approachProgress(chartTimeMilliseconds);
    const double fadeProgress = std::clamp(progress / 0.1, 0.0, 1.0);
    return (3.0 - 2.0 * fadeProgress) * fadeProgress * fadeProgress;
}

double ConveyorView::scrollCoordinateAt(const double chartTimeMilliseconds) const {
    if (chartTimeMilliseconds < 0.0) {
        return chartTimeMilliseconds * m_scrollPoints.front().speed;
    }
    const auto end = std::upper_bound(m_scrollPoints.cbegin(), m_scrollPoints.cend(), chartTimeMilliseconds,
        [](const double time, const ScrollPoint& point) { return time < point.time; });
    const ScrollPoint& point = *std::prev(end);
    return point.coordinate + (chartTimeMilliseconds - point.time) * point.speed;
}

double ConveyorView::scrollDistanceTo(const qint64 chartTimeMilliseconds) const {
    return scrollCoordinateAt(chartTimeMilliseconds)
        - m_frameScrollCoordinate;
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
            const double startProgress = std::clamp(projectionProgress(clippedStart), 0.0, 1.0);
            const double endProgress = std::clamp(projectionProgress(clippedEnd), 0.0, 1.0);
            if (startProgress != endProgress) {
                painter.drawPath(ConveyorGeometry::floorLane(lane, startProgress, endProgress));
            }
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
    if (!m_playtest && rawEnd <= m_playbackPositionMilliseconds) {
        return;
    }
    const qint64 noteStart = m_playtest ? m_playtestNoteStarts.value(note.id, note.startMilliseconds)
        : std::max(note.startMilliseconds, m_playbackPositionMilliseconds);
    if (noteStart >= rawEnd) {
        return;
    }
    const FloorSpan startSpan = floorSpan(note.side, note.startX, note.startWidth);
    const FloorSpan endSpan = floorSpan(note.side, note.endX, note.endWidth);
    const QPainterPath shape = ConveyorGeometry::floorPath({
        {startSpan.left, projectionProgress(noteStart)}, {startSpan.right, projectionProgress(noteStart)},
        {endSpan.right, projectionProgress(rawEnd)}, {endSpan.left, projectionProgress(rawEnd)}}, note.side);
    const bool outerHold = note.kind == NoteKind::Hold
        && (note.side == FloorSide::LeftOuter || note.side == FloorSide::RightOuter);
    const QColor color = (m_playtestNotes.value(note.id) & 2) != 0
        ? floorColor(note).lighter(outerHold ? 110 : 150) : floorColor(note);

    painter.save();
    painter.setOpacity(painter.opacity() * noteOpacity(noteStart));
    const QColor contour = palette::floorNoteContour;
    painter.setPen(QPen(contour, kNoteContourWidth));
    painter.setBrush(color);
    painter.drawPath(shape);
    painter.restore();
}

void ConveyorView::drawFlick(QPainter& painter, const ChartNote& note) {
    const qint64 drawTime = note.startMilliseconds;
    if (!ConveyorGeometry::skyVisible(projectionProgress(drawTime))) {
        return;
    }
    const auto pointAt = [this, drawTime](const double coordinate) {
        return skyPointAt(coordinate, drawTime);
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
    painter.setOpacity(painter.opacity() * noteOpacity(drawTime));
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
    const qint64 timeMilliseconds, const QVector<infalsus::FieldSegment>& sharedSegments, const QColor& color) {
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
    const bool held = (m_playtestNotes.value(note.id) & 2) != 0;
    const bool missing = (m_playtestNotes.value(note.id) & 4) != 0;
    const QColor fieldColor = missing ? palette::outerNoteRed : palette::skyArea;
    if (!m_playtest && note.endMilliseconds <= m_playbackPositionMilliseconds) {
        return;
    }
    const qint64 clippedStart = m_playtest ? m_playtestNoteStarts.value(note.id, note.startMilliseconds)
        : std::max(note.startMilliseconds, m_playbackPositionMilliseconds);
    const qint64 clippedEnd = note.endMilliseconds;
    if (clippedStart >= clippedEnd) {
        return;
    }
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

    const QColor color = palette::withAlpha(fieldColor, held ? 170 : kSkyFillAlpha);
    const QColor contourColor = fieldColor.lighter(115);
    const bool originalStartVisible = clippedStart == note.startMilliseconds;
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
        m_sharedFieldEnds.value(index), contourColor);
    if (originalStartVisible) {
        drawFieldBoundary(painter, fieldLeftEdge(note, true), fieldRightEdge(note, true), note.startMilliseconds,
            m_sharedFieldStarts.value(index), contourColor);
    } else {
        painter.setPen(QPen(contourColor, kSkyContourWidth));
        painter.drawPath(ConveyorGeometry::skyPath({leftBoundary.front(), rightBoundary.front()}, false));
    }
    painter.restore();
}

void ConveyorView::ensureNoteIndexes() {
    if (!m_noteIndexesDirty) {
        return;
    }
    for (auto& indexes : m_noteIndexes) {
        indexes.clear();
    }
    m_noteScrollRanges.resize(m_chart.notes.size());
    for (int index = 0; index < m_chart.notes.size(); ++index) {
        const ChartNote& note = m_chart.notes.at(index);
        const int sideIndex = static_cast<int>(note.side);
        if (sideIndex >= 0 && sideIndex < static_cast<int>(m_noteIndexes.size())) {
            m_noteIndexes[sideIndex].append(index);
        }
        const qint64 end = note.kind == NoteKind::Tap ? note.startMilliseconds + kTapDepthMilliseconds
            : note.endMilliseconds;
        const double first = scrollCoordinateAt(note.startMilliseconds);
        const double last = scrollCoordinateAt(end);
        ScrollRange range{std::min(first, last), std::max(first, last)};
        auto point = std::upper_bound(m_scrollPoints.cbegin(), m_scrollPoints.cend(), note.startMilliseconds,
            [](const double time, const ScrollPoint& candidate) { return time < candidate.time; });
        for (; point != m_scrollPoints.cend() && point->time < end; ++point) {
            range.minimum = std::min(range.minimum, point->coordinate);
            range.maximum = std::max(range.maximum, point->coordinate);
        }
        m_noteScrollRanges[index] = range;
    }
    for (auto& indexes : m_noteIndexes) {
        std::stable_sort(indexes.begin(), indexes.end(), [this](const int firstIndex, const int secondIndex) {
            const auto& first = m_chart.notes.at(firstIndex);
            const auto& second = m_chart.notes.at(secondIndex);
            if (first.startMilliseconds != second.startMilliseconds) {
                return first.startMilliseconds < second.startMilliseconds;
            }
            return first.endMilliseconds < second.endMilliseconds;
        });
    }
    m_noteIndexesDirty = false;
}

void ConveyorView::drawChart(QPainter& painter) {
    ensureFieldBoundaries();
    ensureNoteIndexes();
    QVector<const ChartNote*> visibleNotes;
    const double approachDistance = approachDistanceMilliseconds();
    const double minimumFloorCoordinate = m_frameScrollCoordinate - approachDistance
        * (ConveyorGeometry::maximumVisibleProgress(FloorSide::Central) - 1.0);
    const double minimumSkyCoordinate = m_frameScrollCoordinate - approachDistance
        * (ConveyorGeometry::maximumVisibleProgress(FloorSide::Sky) - 1.0);
    for (const auto& sideNotes : m_noteIndexes) {
        for (const int index : sideNotes) {
            const ChartNote* note = &m_chart.notes.at(index);
            const bool completed = note->endMilliseconds <= m_playbackPositionMilliseconds;
            if (m_playtest && (!m_playtestNotes.contains(note->id) || (m_playtestNotes.value(note->id) & 1) != 0)) {
                continue;
            }
            const ScrollRange& range = m_noteScrollRanges.at(index);
            const double minimumCoordinate = note->side == FloorSide::Sky ? minimumSkyCoordinate : minimumFloorCoordinate;
            if (range.maximum < minimumCoordinate) {
                continue;
            }
            if (!m_playtest && completed && (note->kind != NoteKind::Sky
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
