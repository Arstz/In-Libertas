#include "gui/widgets/timeline_widget.h"

#include "gui/app/game_palette.h"

#include <QtGui/QMouseEvent>
#include <QtGui/QPainter>

#include <algorithm>

namespace infalsus::gui {

TimelineWidget::TimelineWidget(QWidget* parent)
    : QWidget(parent) {
    setFixedHeight(kWidgetHeight);
    setCursor(Qt::PointingHandCursor);
}

void TimelineWidget::setDuration(const qint64 durationMilliseconds) {
    m_durationMilliseconds = std::max(durationMilliseconds, qint64(1));
    update();
}

void TimelineWidget::setPlaybackPosition(const qint64 positionMilliseconds) {
    m_playbackPositionMilliseconds = std::clamp(positionMilliseconds, qint64(0), m_durationMilliseconds);
    update();
}

void TimelineWidget::setTimingPoints(QVector<TimingPoint> timingPoints) {
    m_timingPoints = std::move(timingPoints);
    update();
}

void TimelineWidget::setSpeedEvents(QVector<SpeedEvent> speedEvents) {
    m_speedEvents = std::move(speedEvents);
    update();
}

void TimelineWidget::mouseMoveEvent(QMouseEvent* event) {
    if (m_dragging) {
        emit playbackPositionRequested(positionForX(event->position().x()));
    }
}

void TimelineWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) {
        return;
    }
    m_dragging = true;
    emit playbackPositionRequested(positionForX(event->position().x()));
}

void TimelineWidget::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        m_dragging = false;
    }
}

void TimelineWidget::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)

    QPainter painter(this);
    painter.fillRect(rect(), palette::chrome);
    const QRectF track = rect().adjusted(8.0, 11.0, -8.0, -11.0);
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::gridLine);
    painter.drawRoundedRect(track, 4.0, 4.0);

    painter.setPen(QPen(palette::outerNoteRed, 2.0));
    for (const TimingPoint& point : m_timingPoints) {
        const double progress = static_cast<double>(point.timeMilliseconds) / m_durationMilliseconds;
        const double x = track.left() + std::clamp(progress, 0.0, 1.0) * track.width();
        painter.drawLine(QPointF(x, 4.0), QPointF(x, height() - 4.0));
    }

    // Type-0 SPC events are conveyor-speed changes.  Keep them distinct from
    // BPM sections: short green bars sit on the timeline track itself.
    painter.setPen(Qt::NoPen);
    painter.setBrush(palette::guideGreen);
    for (const SpeedEvent& event : m_speedEvents) {
        const double progress = static_cast<double>(event.timeMilliseconds) / m_durationMilliseconds;
        const double x = track.left() + std::clamp(progress, 0.0, 1.0) * track.width();
        painter.drawRect(QRectF(x - 2.0, track.center().y(), 4.0, track.height() * 0.5));
    }

    const double playheadProgress = static_cast<double>(m_playbackPositionMilliseconds) / m_durationMilliseconds;
    const double playheadX = track.left() + std::clamp(playheadProgress, 0.0, 1.0) * track.width();
    painter.setPen(QPen(palette::text, 2.0));
    painter.drawLine(QPointF(playheadX, 2.0), QPointF(playheadX, height() - 2.0));
}

qint64 TimelineWidget::positionForX(const double x) const {
    const double width = std::max(1, this->width() - 16);
    const double progress = std::clamp((x - 8.0) / width, 0.0, 1.0);

    return static_cast<qint64>(progress * m_durationMilliseconds);
}

} // namespace infalsus::gui
