#pragma once

#include "gui/state/editor_state.h"
#include "gui/app/handling_settings.h"
#include "core/zone_groups.h"

#include <QtCore/QByteArray>
#include <QtCore/QBuffer>
#include <QtCore/QSet>
#include <QtGui/QColor>
#include <QtGui/QCursor>
#include <QtGui/QPainterPath>
#include <QtOpenGLWidgets/QOpenGLWidget>

class QPainter;
class QMouseEvent;
class QAudioBuffer;
class QAudioDecoder;
class QWheelEvent;

namespace infalsus::gui {

class FlatView final : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit FlatView(QWidget* parent = nullptr);

    void setChart(const ChartData& chart);
    void setPlaybackPosition(qint64 positionMilliseconds);
    void setPlayheadColor(const QColor& color);
    void setInvertMousewheelScroll(bool inverted);
    void setMode(FlatViewMode mode);
    void setTool(EditorTool tool);
    void setGridDuration(qint64 durationMilliseconds);
    void setDivisor(int divisor);
    void setTimingPoints(QVector<TimingPoint> timingPoints);
    void setLaneEvents(QVector<LaneEvent> laneEvents);
    void setSelectedHitObjects(QVector<int> indexes);
    void setAudioSource(const QString& audioPath);
    void setAudioData(QByteArray audioData, const QString& fileName);
    void zoom(double steps);
    [[nodiscard]] int playheadViewportY() const;
    void addHitObject(int index, const ChartNote& hitObject);
    void removeHitObject(int index);
    void updateHitObject(int index, const ChartNote& hitObject);
    void updateHitObjects(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects);

signals:
    void playbackPositionRequested(qint64 positionMilliseconds);
    void divisorSeekRequested(int direction);
    void divisorIncreaseRequested();
    void divisorDecreaseRequested();
    void hitObjectAddRequested(const ChartNote& hitObject);
    void hitObjectRemoveRequested(int index);
    void hitObjectCreationCancelled(int index);
    void hitObjectMoveStarted(int index);
    void hitObjectMoveRequested(int index, const ChartNote& hitObject);
    void hitObjectMoveFinished();
    void hitObjectBatchMoveStarted(QVector<int> indexes);
    void hitObjectBatchMoveRequested(QVector<int> indexes, QVector<ChartNote> hitObjects);
    void hitObjectBatchMoveFinished();
    void hitObjectSelectionRequested(QVector<int> indexes);
    void hitObjectsCopyRequested(QVector<ChartNote> hitObjects);
    void waveformError(const QString& message);

protected:
    bool event(QEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintGL() override;
    void wheelEvent(QWheelEvent* event) override;

private:
    enum class ZoneControlKind {
        None,
        StartLeft,
        StartRight,
        EndLeft,
        EndRight,
        StartTime,
        EndTime,
        LeftSide,
        RightSide,
    };

    struct ZoneControl {
        ZoneControlKind kind = ZoneControlKind::None;
        int hitObjectIndex = -1;
    };

    struct ZoneSegmentAnchor {
        int hitObjectIndex = -1;
        bool atStart = true;
    };

    enum class FlickControlKind {
        None,
        Left,
        Right,
    };

    struct FlickControl {
        FlickControlKind kind = FlickControlKind::None;
        int hitObjectIndex = -1;
    };

    [[nodiscard]] QRectF groundArea() const;
    [[nodiscard]] qint64 timeAtY(double y) const;
    [[nodiscard]] qint64 snappedTimeAtY(double y) const;
    [[nodiscard]] int laneAt(const QPointF& position) const;
    [[nodiscard]] int hitObjectAt(const QPointF& position) const;
    [[nodiscard]] int holdEndHitObjectAt(const QPointF& position) const;
    [[nodiscard]] int zoneHitObjectAt(const QPointF& position) const;
    [[nodiscard]] int flickHitObjectAt(const QPointF& position) const;
    [[nodiscard]] QVector<int> hitObjectsAt(const QPointF& position) const;
    [[nodiscard]] QVector<int> hitObjectsInRect(const QRectF& rectangle, Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] QVector<int> selectionForClick(int hitObjectIndex, Qt::KeyboardModifiers modifiers) const;
    [[nodiscard]] QVector<ChartNote> hitObjectsForSelectionMove(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] FlickControl flickControlAt(const QPointF& position) const;
    [[nodiscard]] ZoneControl zoneControlAt(const QPointF& position) const;
    [[nodiscard]] ZoneSegmentAnchor zoneSegmentAnchorAt(const QPointF& position) const;
    [[nodiscard]] ChartNote hitObjectForPlacement(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote floorHitObjectForDrag(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote floorHitObjectForMove(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote holdHitObjectForEndMove(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote flickHitObjectForDrag(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote flickHitObjectForMove(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote flickHitObjectForControlMove(const QPointF& position) const;
    [[nodiscard]] ChartNote zoneHitObjectForDrag(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote zoneHitObjectForMove(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote zoneHitObjectForControlMove(const QPointF& position) const;
    [[nodiscard]] QVector<int> linkedZoneIndexes() const;
    [[nodiscard]] QVector<ChartNote> zoneHitObjectsForControlMove(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] bool zoneExtensionExists(const QPointF& position, bool snapToGrid) const;
    [[nodiscard]] ChartNote snapZoneToAdjacentSegment(ChartNote hitObject, bool* snapped) const;
    [[nodiscard]] bool canInteractGround() const;
    [[nodiscard]] bool canInteractSky() const;
    [[nodiscard]] bool canPlaceGround() const;
    [[nodiscard]] bool canPlaceSky() const;
    void invalidateZoneGroups();
    void ensureZoneGroups() const;
    void resetWaveformAudio();
    void appendWaveformAudio(const QAudioBuffer& buffer);
    void beginDragNavigation(const QPointF& position);
    void updateDragNavigation(const QPointF& position);
    void finishDragNavigation();
    [[nodiscard]] double timeToY(qint64 timeMilliseconds) const;
    [[nodiscard]] double playheadY() const;
    [[nodiscard]] double flickTriangleHeight() const;
    void drawWaveform(QPainter& painter, const QRectF& area) const;
    void drawDividers(QPainter& painter, const QRectF& area) const;
    void drawGround(QPainter& painter, const QRectF& area, int opacity) const;
    void drawSky(QPainter& painter, const QRectF& area, int opacity) const;
    void drawFlickControls(QPainter& painter, const QRectF& area, const ChartNote& hitObject) const;
    void drawZoneControls(QPainter& painter, const QRectF& area, const ChartNote& hitObject) const;
    void drawSelectionRectangle(QPainter& painter) const;
    [[nodiscard]] bool shouldDrawZoneControls(int hitObjectIndex) const;
    [[nodiscard]] bool shouldDrawFlickControls(int hitObjectIndex) const;

    ChartData m_chart;
    QColor m_playheadColor;
    qint64 m_playbackPositionMilliseconds = 0;
    qint64 m_gridDurationMilliseconds = 500;
    double m_pixelsPerSecond = 120.0;
    FlatViewMode m_mode = FlatViewMode::Both;
    EditorTool m_tool = EditorTool::Place;
    QVector<TimingPoint> m_timingPoints;
    QVector<LaneEvent> m_laneEvents;
    QVector<int> m_selectedHitObjects;
    QSet<int> m_selectedHitObjectIndexes;
    mutable QVector<ZoneGroup> m_zoneGroups;
    mutable QVector<QPainterPath> m_zoneGroupContours;
    mutable QVector<QPainterPath> m_zoneGroupJoints;
    mutable bool m_zoneGroupsDirty = true;
    QVector<QPointF> m_waveformPeaks;
    ChartNote m_dragOriginal;
    QAudioDecoder* m_waveformDecoder = nullptr;
    QBuffer m_waveformBuffer;
    QByteArray m_waveformAudioData;
    QPointF m_pressPosition;
    QPointF m_navigationLastPosition;
    QCursor m_navigationPreviousCursor;
    double m_navigationPositionMilliseconds = 0.0;
    double m_waveformMillisecondsPerPeak = 0.0;
    int m_waveformFramesPerPeak = 1;
    int m_waveformFramesInPeak = 0;
    double m_waveformMinimum = 0.0;
    double m_waveformMaximum = 0.0;
    Qt::KeyboardModifiers m_pressModifiers = Qt::NoModifier;
    int m_dragHitObjectIndex = -1;
    int m_divisor = 4;
    ZoneControl m_dragZoneControl;
    FlickControl m_dragFlickControl;
    ZoneSegmentAnchor m_zoneSegmentAnchor;
    QVector<int> m_dragLinkedZoneIndexes;
    QVector<ChartNote> m_dragLinkedZoneOriginals;
    QVector<int> m_dragSelectionIndexes;
    QVector<ChartNote> m_dragSelectionOriginals;
    qint64 m_dragSelectionEarliestStart = 0;
    QVector<int> m_clickCycleIndexes;
    QPointF m_clickCyclePosition;
    int m_clickCycleIndex = -1;
    bool m_dragMoved = false;
    bool m_draggingHoldEnd = false;
    bool m_draggingHitObject = false;
    bool m_placingHitObject = false;
    bool m_selectionOnlyClick = false;
    bool m_marqueeSelecting = false;
    bool m_movingSelection = false;
    bool m_invertMousewheelScroll = kDefaultInvertMousewheelScroll;
    bool m_dragNavigating = false;
    bool m_navigationHadCursor = false;
};

} // namespace infalsus::gui
