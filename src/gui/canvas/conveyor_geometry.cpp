#include "gui/canvas/conveyor_geometry.h"

#include <QtCore/QLineF>
#include <QtCore/QSizeF>

#include <algorithm>
#include <array>
#include <cmath>

namespace {

constexpr double kCanvasWidth = 2560.0;
constexpr double kCanvasHeight = 1440.0;
constexpr double kMinimumCoordinate = -0.125;
constexpr double kCoordinateStep = 0.25;
constexpr double kMaximumCoordinate = 1.375;
constexpr std::size_t kFloorBoundaryCount = 7;

const std::array<QPointF, kFloorBoundaryCount> kNearFloorBoundaries{
    QPointF(160.0, 1050.0),
    QPointF(540.0, 1288.0),
    QPointF(910.0, 1288.0),
    QPointF(1280.0, 1288.0),
    QPointF(1650.0, 1288.0),
    QPointF(2020.0, 1288.0),
    QPointF(2400.0, 1050.0)
};

const std::array<QPointF, kFloorBoundaryCount> kFarFloorBoundaries{
    QPointF(1200.0, 127.0),
    QPointF(1220.0, 150.0),
    QPointF(1250.0, 150.0),
    QPointF(1280.0, 150.0),
    QPointF(1310.0, 150.0),
    QPointF(1340.0, 150.0),
    QPointF(1360.0, 127.0)
};

[[nodiscard]] QPointF interpolate(const QPointF& first, const QPointF& second, const double progress) {
    return first + (second - first) * progress;
}

[[nodiscard]] QPointF lineIntersection(const QLineF& first, const QLineF& second) {
    QPointF intersection;
    first.intersects(second, &intersection);
    return intersection;
}

[[nodiscard]] double perspectiveScreenProgress(
    const QPointF& farPoint,
    const QPointF& nearPoint,
    const QPointF& vanishingPoint,
    const double depthProgress) {
    const QPointF farOffset = farPoint - vanishingPoint;
    const QPointF nearOffset = nearPoint - vanishingPoint;
    const double nearLength = std::hypot(nearOffset.x(), nearOffset.y());
    const double farLength = std::hypot(farOffset.x(), farOffset.y());
    const double farScale = nearLength > 0.0 ? farLength / nearLength : 0.0;
    const double clampedDepthProgress = std::clamp(depthProgress, 0.0, 1.0);
    const double distance = (1.0 - clampedDepthProgress) / std::max(farScale, 0.0001) + clampedDepthProgress;
    const double projectedScale = 1.0 / distance;

    return std::clamp((projectedScale - farScale) / (1.0 - farScale), 0.0, 1.0);
}

[[nodiscard]] QPointF perspectiveInterpolate(
    const QPointF& farPoint,
    const QPointF& nearPoint,
    const QPointF& vanishingPoint,
    const double depthProgress) {
    return interpolate(farPoint, nearPoint, perspectiveScreenProgress(farPoint, nearPoint, vanishingPoint, depthProgress));
}

[[nodiscard]] QPointF floorVanishingPoint() {
    return lineIntersection(
        QLineF(kNearFloorBoundaries[1], kFarFloorBoundaries[1]),
        QLineF(kNearFloorBoundaries[5], kFarFloorBoundaries[5]));
}

[[nodiscard]] QPointF floorBoundaryPoint(const double boundaryCoordinate, const double depthProgress) {
    const double slot = std::clamp(
        (boundaryCoordinate - kMinimumCoordinate) / kCoordinateStep,
        0.0,
        static_cast<double>(kFloorBoundaryCount - 1));
    const std::size_t lowerIndex = static_cast<std::size_t>(std::floor(slot));
    const std::size_t upperIndex = std::min(lowerIndex + 1, kFloorBoundaryCount - 1);
    const double localProgress = slot - static_cast<double>(lowerIndex);
    const QPointF farPoint = interpolate(kFarFloorBoundaries[lowerIndex], kFarFloorBoundaries[upperIndex], localProgress);
    const QPointF nearPoint = interpolate(kNearFloorBoundaries[lowerIndex], kNearFloorBoundaries[upperIndex], localProgress);

    return perspectiveInterpolate(farPoint, nearPoint, floorVanishingPoint(), depthProgress);
}

} // namespace

QSizeF ConveyorGeometry::nativeSize() {
    return QSizeF(kCanvasWidth, kCanvasHeight);
}

QPolygonF ConveyorGeometry::floorOutline() {
    return QPolygonF{
        kNearFloorBoundaries.front(),
        kNearFloorBoundaries[1],
        kNearFloorBoundaries[5],
        kNearFloorBoundaries.back(),
        kFarFloorBoundaries.back(),
        kFarFloorBoundaries[5],
        kFarFloorBoundaries[1],
        kFarFloorBoundaries.front()
    };
}

QPolygonF ConveyorGeometry::floorReceiver() {
    return QPolygonF(kNearFloorBoundaries.begin(), kNearFloorBoundaries.end());
}

QPolygonF ConveyorGeometry::floorLane(const std::size_t laneIndex) {
    const std::size_t nextLaneIndex = std::min(laneIndex + 1, kFloorBoundaryCount - 1);

    return QPolygonF{
        kNearFloorBoundaries[laneIndex],
        kNearFloorBoundaries[nextLaneIndex],
        kFarFloorBoundaries[nextLaneIndex],
        kFarFloorBoundaries[laneIndex]
    };
}

QLineF ConveyorGeometry::skyReceiver() {
    return QLineF(QPointF(466.0, 920.0), QPointF(2094.0, 920.0));
}

QLineF ConveyorGeometry::skyHorizon() {
    return QLineF(QPointF(1210.0, 113.0), QPointF(1350.0, 113.0));
}

QPointF ConveyorGeometry::floorPoint(const double coordinate, const double depthProgress) {
    return floorBoundaryPoint(coordinate, std::clamp(depthProgress, 0.0, 1.0));
}

QPointF ConveyorGeometry::skyPoint(const double coordinate, const double depthProgress) {
    const QLineF farLine = skyHorizon();
    const QLineF nearLine = skyReceiver();
    const double horizontalProgress = std::clamp(coordinate, 0.0, 1.0);
    const QPointF farPoint = interpolate(farLine.p1(), farLine.p2(), horizontalProgress);
    const QPointF nearPoint = interpolate(nearLine.p1(), nearLine.p2(), horizontalProgress);
    const QPointF vanishingPoint = lineIntersection(
        QLineF(nearLine.p1(), farLine.p1()),
        QLineF(nearLine.p2(), farLine.p2()));

    return perspectiveInterpolate(farPoint, nearPoint, vanishingPoint, depthProgress);
}
