#pragma once

#include <QtCore/QLineF>
#include <QtCore/QPointF>
#include <QtCore/QSizeF>
#include <QtGui/QPolygonF>

#include <array>

class ConveyorGeometry {
public:
    [[nodiscard]] static QSizeF nativeSize();
    [[nodiscard]] static QPolygonF floorOutline();
    [[nodiscard]] static QPolygonF floorReceiver();
    [[nodiscard]] static QPolygonF floorLane(std::size_t laneIndex);
    [[nodiscard]] static QLineF skyReceiver();
    [[nodiscard]] static QLineF skyHorizon();
    [[nodiscard]] static QPointF floorPoint(double coordinate, double approachProgress);
    [[nodiscard]] static QPointF skyPoint(double coordinate, double approachProgress);
};
