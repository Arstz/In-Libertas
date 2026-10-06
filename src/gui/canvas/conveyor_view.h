#pragma once

#include "core/chart_types.h"
#include "core/chart_project.h"

#include <QtCore/QPointF>
#include <QtGui/QImage>
#include <QtOpenGLWidgets/QOpenGLWidget>

class QPainter;

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
    void setPlaybackPosition(qint64 positionMilliseconds);
    void addHitObject(int index, const ChartNote& hitObject);
    void removeHitObject(int index);
    void updateHitObject(int index, const ChartNote& hitObject);
    void updateHitObjects(const QVector<int>& indexes, const QVector<ChartNote>& hitObjects);

protected:
    void paintGL() override;

private:
    [[nodiscard]] double approachProgress(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double approachDistanceMilliseconds() const;
    [[nodiscard]] double noteOpacity(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double scrollCoordinateAt(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double scrollDistanceTo(qint64 chartTimeMilliseconds) const;
    [[nodiscard]] bool isVisible(qint64 startMilliseconds, qint64 endMilliseconds) const;
    [[nodiscard]] QPointF floorPointAt(double coordinate, qint64 chartTimeMilliseconds) const;
    [[nodiscard]] QPointF skyPointAt(double coordinate, qint64 chartTimeMilliseconds) const;
    [[nodiscard]] double easingFor(std::uint32_t auxiliary, bool rightSide, double progress) const;
    void drawConveyor(QPainter& painter);
    void drawDivisors(QPainter& painter);
    void drawFloorNote(QPainter& painter, const ChartNote& note);
    void drawFlick(QPainter& painter, const ChartNote& note);
    void drawSkyZone(QPainter& painter, const ChartNote& note);
    void drawChart(QPainter& painter);
    void drawSongCard(QPainter& painter) const;

    ChartData m_chart;
    QVector<infalsus::TimingPoint> m_timingPoints;
    QVector<infalsus::LaneEvent> m_laneEvents;
    QVector<infalsus::SpeedEvent> m_speedEvents;
    infalsus::ChartMetadata m_cardMetadata;
    infalsus::Difficulty m_cardDifficulty = infalsus::Difficulty::Minimal;
    infalsus::DifficultyMetadata m_cardDifficultyMetadata;
    QImage m_cardJacket;
    qint64 m_playbackPositionMilliseconds = 0;
    double m_noteSpeed = 2.5;
};
