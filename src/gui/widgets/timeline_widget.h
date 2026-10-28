#pragma once

#include "core/chart_project.h"

#include <QtWidgets/QWidget>
#include <QtGui/QPixmap>

class QMouseEvent;
class QPainter;

namespace infalsus::gui {

class TimelineWidget final : public QWidget {
    Q_OBJECT

public:
    static constexpr int kWidgetHeight = 56;

    explicit TimelineWidget(QWidget* parent = nullptr);

    void setDuration(qint64 durationMilliseconds);
    void setPlaybackPosition(qint64 positionMilliseconds);
    void setTimingPoints(QVector<TimingPoint> timingPoints);
    void setSpeedEvents(QVector<SpeedEvent> speedEvents);

signals:
    void playbackPositionRequested(qint64 positionMilliseconds);

protected:
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    [[nodiscard]] qint64 positionForX(double x) const;
    void ensureEventMarkers();

    qint64 m_durationMilliseconds = 1;
    qint64 m_playbackPositionMilliseconds = 0;
    QVector<TimingPoint> m_timingPoints;
    QVector<SpeedEvent> m_speedEvents;
    QPixmap m_eventMarkers;
    bool m_eventMarkersDirty = true;
    bool m_dragging = false;
};

} // namespace infalsus::gui
