#pragma once

#include "core/chart_types.h"
#include "core/chart_project.h"
#include "core/field_groups.h"
#include "core/playtest_session.h"

#include <QtCore/QPointF>
#include <QtCore/QHash>
#include <QtCore/QSet>
#include <QtGui/QImage>
#include <QtGui/QPainterPath>
#include <QtOpenGLWidgets/QOpenGLWidget>

class QPainter;
class QColor;

class ConveyorView final : public QOpenGLWidget {
    Q_OBJECT

public:
    explicit ConveyorView(QWidget* parent = nullptr);

    void setChart(const ChartData& chart);
    void setTimingPoints(QVector<infalsus::TimingPoint> timingPoints);
    void setLaneEvents(QVector<infalsus::LaneEvent> laneEvents);
    void setSpeedEvents(QVector<infalsus::SpeedEvent> speedEvents);
    void setSongCard(const infalsus::ChartMetadata& metadata, infalsus::Difficulty difficulty,
        infalsus::DifficultyMetadata difficultyMetadata, QImage jacket);
    void setNoteSpeed(double noteSpeed);
    void setPlaytestLaneKeys(const std::array<Qt::Key, 6>& keys);
    void setPlaytestAutoplayKey(Qt::Key key);
    void setPlaybackPosition(qint64 positionMilliseconds);
    void setPlaytestFrame(const infalsus::PlaytestSession& session, const QString& status,
        const QVector<infalsus::PlaytestJudgement>& judgements);
    void endPlaytest();
    [[nodiscard]] double gameplayMouseWidth() const;
    void addHitObject(int index, const ChartNote& hitObject);
    void removeHitObject(int index);
    void updateHitObject(int index, const ChartNote& hitObject);
    void updateHitObjects(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects);

protected:
    void paintGL() override;

private:
    struct MeasureMarker {
        double timeMilliseconds;
        double coordinate;
    };
    struct ScrollPoint {
        qint64 time;
        double coordinate;
        double speed;
    };
    struct ScrollRange {
        double minimum;
        double maximum;
    };

    [[nodiscard]] double approachProgress(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double projectionProgress(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double approachDistanceMilliseconds() const;
    [[nodiscard]] double noteOpacity(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double scrollCoordinateAt(double chartTimeMilliseconds) const;
    [[nodiscard]] double scrollDistanceTo(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] ScrollRange scrollRange(qint64 startMilliseconds, qint64 endMilliseconds) const;
    [[nodiscard]] bool isVisible(qint64 startMilliseconds, qint64 endMilliseconds) const;
    [[nodiscard]] QPointF skyPointAt(double coordinate, qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double easingFor(std::uint32_t auxiliary, bool rightSide, double progress) const;
    void drawConveyor(QPainter& painter);
    void ensureMeasureMarkers();
    void drawDivisors(QPainter& painter);
    void drawFloorNote(QPainter& painter, const ChartNote& note);
    void drawFlick(QPainter& painter, const ChartNote& note);
    void ensureFieldBoundaries();
    void invalidateFieldBoundary(int index, const ChartNote& hitObject);
    void indexHitObject(int index);
    void drawFieldBoundary(QPainter& painter, double left, double right, qint64 timeMilliseconds,
        const QVector<infalsus::FieldSegment>& sharedSegments, const QColor& color);
    void drawSkyField(QPainter& painter, const ChartNote& note, int index);
    void drawChart(QPainter& painter);
    void ensureNoteIndexes();
    void drawSongCard(QPainter& painter) const;
    void drawCompactSongCard(QPainter& painter, const QRectF& card, const QPainterPath& conveyor) const;
    void drawScoreCard(QPainter& painter) const;
    void drawControlHints(QPainter& painter) const;
    void drawPlaytestInput(QPainter& painter) const;
    void drawPlaytestCombo(QPainter& painter) const;
    void drawPlaytestAutoplay(QPainter& painter) const;
    void drawPlaytestJudgements(QPainter& painter) const;
    void drawPlaytestOverlay(QPainter& painter) const;

    ChartData m_chart;
    QHash<int, QVector<infalsus::FieldSegment>> m_sharedFieldStarts;
    QHash<int, QVector<infalsus::FieldSegment>> m_sharedFieldEnds;
    QVector<infalsus::FieldGroup> m_fieldGroups;
    QVector<int> m_fieldGroupIndexes;
    QSet<int> m_dirtyFieldGroups;
    QVector<infalsus::TimingPoint> m_timingPoints;
    QVector<infalsus::LaneEvent> m_laneEvents;
    QVector<infalsus::SpeedEvent> m_speedEvents;
    QVector<ScrollPoint> m_scrollPoints{{0, 0.0, 1.0}};
    QVector<ScrollRange> m_scrollRangeTree{{0.0, 0.0}, {0.0, 0.0}};
    std::array<QVector<int>, 4> m_noteIndexes;
    QVector<ScrollRange> m_noteScrollRanges;
    QVector<MeasureMarker> m_measureMarkers;
    infalsus::ChartMetadata m_cardMetadata;
    infalsus::Difficulty m_cardDifficulty = infalsus::Difficulty::Minimal;
    infalsus::DifficultyMetadata m_cardDifficultyMetadata;
    QImage m_cardJacket;
    qint64 m_playbackPositionMilliseconds = 0;
    qint64 m_measureChartEndMilliseconds = 0;
    double m_noteSpeed = 2.5;
    double m_frameScrollCoordinate = 0.0;
    bool m_noteIndexesDirty = true;
    bool m_fieldBoundariesDirty = true;
    bool m_measureMarkersDirty = true;
    bool m_playtest = false;
    QHash<std::uint64_t, std::uint8_t> m_playtestNotes;
    QHash<std::uint64_t, qint64> m_playtestNoteStarts;
    infalsus::PlaytestResult m_playtestResult;
    QString m_comboText;
    QPainterPath m_autoplayLabel;
    bool m_playtestAutoplay = false;
    QString m_scoreText;
    std::array<QPainterPath, 6> m_keyIndicatorGlyphs;
    std::array<bool, 6> m_playtestHeld{};
    double m_playtestCursor = 0.5;
    double m_sweepLeft = 0.5;
    double m_sweepRight = 0.5;
    QString m_playtestStatus;
    QString m_autoplayHint;
    std::array<QImage, 7> m_judgementImages;
    QVector<infalsus::PlaytestJudgement> m_judgements;
};
