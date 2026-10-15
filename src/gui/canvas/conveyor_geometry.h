#pragma once

#include "core/chart_types.h"

#include <QtCore/QLineF>
#include <QtCore/QPointF>
#include <QtCore/QSizeF>
#include <QtGui/QPolygonF>
#include <QtGui/QPainterPath>

class ConveyorGeometry {
public:
    [[nodiscard]] static QSizeF nativeSize();
    [[nodiscard]] static QPolygonF floorOutline();
    [[nodiscard]] static QPolygonF floorReceiver();
    [[nodiscard]] static QPainterPath floorLane(std::size_t laneIndex,
        double startProgress = 1.0, double endProgress = 0.0);
    [[nodiscard]] static QLineF skyReceiver();
    [[nodiscard]] static QLineF skyHorizon();
    [[nodiscard]] static QPointF floorPoint(double coordinate, double approachProgress);
    [[nodiscard]] static QPointF skyPoint(double coordinate, double approachProgress);
    [[nodiscard]] static bool skyVisible(double depthProgress);
    [[nodiscard]] static QPainterPath floorPath(const QPolygonF& points, FloorSide side, bool closed = true);
    [[nodiscard]] static QPainterPath skyPath(const QPolygonF& points, bool closed = true);
    [[nodiscard]] static QPainterPath measureMarker(double normalizedDistance);
};
