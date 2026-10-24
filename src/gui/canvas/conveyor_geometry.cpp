#include "gui/canvas/conveyor_geometry.h"

#include <QtCore/QSizeF>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kCanvasWidth = 2560.0;
constexpr double kCanvasHeight = 1440.0;
constexpr double kCameraNearPlane = 0.3;
constexpr double kCameraFarPlane = 1000.0;
constexpr double kCameraDistance = 10.0;
constexpr double kCameraFieldOfView = 60.000003814697266;
constexpr double kTrackRotationX = -0.21072766184806824;
constexpr double kTrackRotationW = 0.9775448441505432;
constexpr double kTrackPositionZ = -7.020000457763672;
constexpr double kReceptorPositionX = 0.002000305103138089;
constexpr double kReceptorPositionY = 0.0011002407409250736;
constexpr double kReceptorPositionZ = -1.4999990463256836;
constexpr double kTrackLength = 20.181811332702637;
constexpr double kTapDepth = 0.2;
constexpr double kFloorWidth = 1.5999999046325684;
constexpr double kSkyWidth = 1.6200000047683716;
constexpr double kSkyHeight = 0.4698088467121124;
constexpr double kOuterWidth = 0.42000005;
constexpr double kLeftPositionX = -0.977308988571167;
constexpr double kLeftPositionY = 0.1696547269821167;
constexpr double kRightPositionX = 0.9775211811065674;
constexpr double kRightPositionY = 0.16967026889324188;
constexpr double kLaneWidth = 0.25;
constexpr double kCentralLeft = 0.125;
constexpr double kCentralRight = 1.125;
constexpr double kCentralCenter = 0.625;
constexpr double kPi = 3.14159265358979323846;
constexpr std::size_t kFloorBoundaryCount = 7;

struct CameraPoint {
    double x;
    double y;
    double z;
};

[[nodiscard]] CameraPoint trackCameraPoint(const double x, const double y, const double z) {
    const double cosine = 1.0 - 2.0 * kTrackRotationX * kTrackRotationX;
    const double sine = 2.0 * kTrackRotationX * kTrackRotationW;

    return {x, cosine * y - sine * z,
        sine * y + cosine * z + kTrackPositionZ + kCameraDistance};
}

[[nodiscard]] CameraPoint cameraPoint(const double x, const double y, const double z) {
    return trackCameraPoint(x + kReceptorPositionX, y + kReceptorPositionY, z);
}

[[nodiscard]] CameraPoint floorCameraPoint(const QPointF& point, const FloorSide side) {
    const double z = kReceptorPositionZ + (1.0 - point.y()) * kTrackLength;
    if (side == FloorSide::LeftOuter || side == FloorSide::RightOuter) {
        const bool left = side == FloorSide::LeftOuter;
        const double boundary = left ? kCentralLeft : kCentralRight;
        const double innerX = (boundary - kCentralCenter) * kFloorWidth;
        const double outerOffset = kOuterWidth * std::sqrt(0.5) * 0.5;
        const double outerX = left ? kLeftPositionX - outerOffset : kRightPositionX + outerOffset;
        const double outerY = (left ? kLeftPositionY : kRightPositionY) + outerOffset;
        const double progress = (point.x() - boundary) / (left ? -kLaneWidth : kLaneWidth);

        return cameraPoint(std::lerp(innerX, outerX, progress), outerY * progress, z);
    }

    return cameraPoint((point.x() - kCentralCenter) * kFloorWidth, 0.0, z);
}

[[nodiscard]] CameraPoint skyCameraPoint(const QPointF& point) {
    return cameraPoint((point.x() - 0.5) * kSkyWidth, kSkyHeight,
        kReceptorPositionZ + (1.0 - point.y()) * kTrackLength);
}

[[nodiscard]] QPointF project(const CameraPoint& point) {
    const double focalLength = kCanvasHeight * 0.5 / std::tan(kCameraFieldOfView * kPi / 360.0);

    return {kCanvasWidth * 0.5 + focalLength * point.x / point.z,
        kCanvasHeight * 0.5 - focalLength * point.y / point.z};
}

[[nodiscard]] QTransform textPlaneProjection(const double height, const double depth) {
    const QPolygonF source{{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    QPolygonF destination;
    for (const QPointF& point : source) {
        destination.append(project(trackCameraPoint(point.x(), height, depth - point.y())));
    }
    QTransform projection;
    QTransform::quadToQuad(source, destination, projection);
    return projection;
}

[[nodiscard]] CameraPoint intersectDepth(const CameraPoint& first, const CameraPoint& second, const double depth) {
    const double progress = (depth - first.z) / (second.z - first.z);

    return {first.x + (second.x - first.x) * progress,
        first.y + (second.y - first.y) * progress, depth};
}

[[nodiscard]] QVector<CameraPoint> clipPolygon(const QVector<CameraPoint>& points, const double depth, const bool near) {
    QVector<CameraPoint> result;
    if (points.isEmpty()) {
        return result;
    }
    CameraPoint previous = points.back();
    bool previousInside = near ? previous.z >= depth : previous.z <= depth;
    for (const CameraPoint& point : points) {
        const bool inside = near ? point.z >= depth : point.z <= depth;
        if (inside != previousInside) {
            result.append(intersectDepth(previous, point, depth));
        }
        if (inside) {
            result.append(point);
        }
        previous = point;
        previousInside = inside;
    }

    return result;
}

[[nodiscard]] QPainterPath projectedPath(QVector<CameraPoint> points, const bool closed) {
    QPainterPath path;
    if (closed) {
        points = clipPolygon(clipPolygon(points, kCameraNearPlane, true), kCameraFarPlane, false);
        if (points.isEmpty()) {
            return path;
        }
        path.moveTo(project(points.front()));
        for (int index = 1; index < points.size(); ++index) {
            path.lineTo(project(points.at(index)));
        }
        path.closeSubpath();
    } else {
        for (int index = 1; index < points.size(); ++index) {
            CameraPoint first = points.at(index - 1);
            CameraPoint second = points.at(index);
            if ((first.z < kCameraNearPlane && second.z < kCameraNearPlane)
                || (first.z > kCameraFarPlane && second.z > kCameraFarPlane)) {
                continue;
            }
            if (first.z < kCameraNearPlane) {
                first = intersectDepth(first, second, kCameraNearPlane);
            } else if (second.z < kCameraNearPlane) {
                second = intersectDepth(first, second, kCameraNearPlane);
            }
            if (first.z > kCameraFarPlane) {
                first = intersectDepth(first, second, kCameraFarPlane);
            } else if (second.z > kCameraFarPlane) {
                second = intersectDepth(first, second, kCameraFarPlane);
            }
            path.moveTo(project(first));
            path.lineTo(project(second));
        }
    }

    return path;
}

[[nodiscard]] FloorSide sideForCoordinate(const double coordinate) {
    if (coordinate < kCentralLeft) {
        return FloorSide::LeftOuter;
    }
    if (coordinate > kCentralRight) {
        return FloorSide::RightOuter;
    }

    return FloorSide::Central;
}

} // namespace

QSizeF ConveyorGeometry::nativeSize() {
    return {kCanvasWidth, kCanvasHeight};
}

QPolygonF ConveyorGeometry::floorOutline() {
    return {floorPoint(-0.125, 1.0), floorPoint(kCentralLeft, 1.0),
        floorPoint(kCentralRight, 1.0), floorPoint(1.375, 1.0),
        floorPoint(1.375, 0.0), floorPoint(kCentralRight, 0.0),
        floorPoint(kCentralLeft, 0.0), floorPoint(-0.125, 0.0)};
}

QPolygonF ConveyorGeometry::floorReceiver() {
    QPolygonF receiver;
    for (std::size_t index = 0; index < kFloorBoundaryCount; ++index) {
        receiver.append(floorPoint(-0.125 + index * kLaneWidth, 1.0));
    }

    return receiver;
}

QPainterPath ConveyorGeometry::floorLane(const std::size_t laneIndex,
    const double startProgress, const double endProgress) {
    const double left = -0.125 + laneIndex * kLaneWidth;
    const double right = left + kLaneWidth;

    return projectedPath({floorCameraPoint({left, startProgress}, sideForCoordinate(left)),
        floorCameraPoint({right, startProgress}, sideForCoordinate(right)),
        floorCameraPoint({right, endProgress}, sideForCoordinate(right)),
        floorCameraPoint({left, endProgress}, sideForCoordinate(left))}, true);
}

QLineF ConveyorGeometry::skyReceiver() {
    return {skyPoint(0.0, 1.0), skyPoint(1.0, 1.0)};
}

QLineF ConveyorGeometry::skyHorizon() {
    return {skyPoint(0.0, 0.0), skyPoint(1.0, 0.0)};
}

QTransform ConveyorGeometry::comboTextProjection() {
    return textPlaneProjection(0.2420000024139881, -0.3600000124424696);
}

QTransform ConveyorGeometry::keyIndicatorProjection(const std::size_t laneIndex) {
    if (laneIndex >= 6) {
        return {};
    }
    const FloorSide side = laneIndex == 0 ? FloorSide::LeftOuter
        : laneIndex == 5 ? FloorSide::RightOuter : FloorSide::Central;
    const double depthScale = side == FloorSide::Central ? 0.1400560438632965 : 0.17507004737854004;
    const QPolygonF source{{-0.5, -0.5}, {0.5, -0.5}, {0.5, 0.5}, {-0.5, 0.5}};
    QPolygonF destination;
    for (const QPointF& point : source) {
        destination.append(project(floorCameraPoint({laneIndex * kLaneWidth + point.x() * kLaneWidth * 0.5,
            1.0 - (0.035 - point.y() * depthScale) / kTrackLength}, side)));
    }
    QTransform projection;
    QTransform::quadToQuad(source, destination, projection);
    return projection;
}

QPointF ConveyorGeometry::floorPoint(const double coordinate, const double depthProgress) {
    CameraPoint point = floorCameraPoint({coordinate, depthProgress}, sideForCoordinate(coordinate));
    point.z = std::clamp(point.z, kCameraNearPlane, kCameraFarPlane);

    return project(point);
}

QPointF ConveyorGeometry::skyPoint(const double coordinate, const double depthProgress) {
    CameraPoint point = skyCameraPoint({coordinate, depthProgress});
    point.z = std::clamp(point.z, kCameraNearPlane, kCameraFarPlane);

    return project(point);
}

QPointF ConveyorGeometry::judgementTextPosition(const double coordinate) {
    CameraPoint point = trackCameraPoint(
        std::lerp(-0.8079997301101685, 0.8120002746582031, std::clamp(coordinate, 0.0, 1.0)),
        0.46980997920036316, kReceptorPositionZ);
    point.y += 0.075000003;
    return project(point);
}

bool ConveyorGeometry::skyVisible(const double depthProgress) {
    const CameraPoint point = skyCameraPoint({0.5, depthProgress});

    return point.z >= kCameraNearPlane && point.z <= kCameraFarPlane;
}

double ConveyorGeometry::maximumVisibleProgress(const FloorSide side) {
    const CameraPoint receiver = side == FloorSide::Sky ? skyCameraPoint({0.5, 1.0})
        : floorCameraPoint({kCentralCenter, 1.0}, FloorSide::Central);
    const double cosine = 1.0 - 2.0 * kTrackRotationX * kTrackRotationX;
    return 1.0 + (receiver.z - kCameraNearPlane) / (kTrackLength * cosine);
}

double ConveyorGeometry::floorTapEndProgress(const double approachProgress) {
    const double depth = kTapDepth * (1.0 + 2.0 * std::abs(1.0 - approachProgress));

    return approachProgress - depth / kTrackLength;
}

QPainterPath ConveyorGeometry::floorPath(const QPolygonF& points, const FloorSide side, const bool closed) {
    QVector<CameraPoint> cameraPoints;
    cameraPoints.reserve(points.size());
    for (const QPointF& point : points) {
        cameraPoints.append(floorCameraPoint(point, side));
    }
    if (closed) {
        const double horizonDepth = floorCameraPoint({kCentralCenter, 0.0}, FloorSide::Central).z;
        cameraPoints = clipPolygon(cameraPoints, horizonDepth, false);
    }

    return projectedPath(std::move(cameraPoints), closed);
}

QPainterPath ConveyorGeometry::skyPath(const QPolygonF& points, const bool closed) {
    QVector<CameraPoint> cameraPoints;
    cameraPoints.reserve(points.size());
    for (const QPointF& point : points) {
        cameraPoints.append(skyCameraPoint(point));
    }

    return projectedPath(std::move(cameraPoints), closed);
}

QPainterPath ConveyorGeometry::measureMarker(const double normalizedDistance) {
    const double halfDepth = (0.005 + 0.075 * std::abs(normalizedDistance)) * 0.5 / kTrackLength;
    const double progress = 1.0 - normalizedDistance;
    QVector<CameraPoint> points;
    points.reserve(kFloorBoundaryCount * 2);
    for (std::size_t index = 0; index < kFloorBoundaryCount; ++index) {
        const double coordinate = -0.125 + index * kLaneWidth;
        points.append(floorCameraPoint({coordinate, progress - halfDepth}, sideForCoordinate(coordinate)));
    }
    for (std::size_t index = kFloorBoundaryCount; index > 0; --index) {
        const double coordinate = -0.125 + (index - 1) * kLaneWidth;
        points.append(floorCameraPoint({coordinate, progress + halfDepth}, sideForCoordinate(coordinate)));
    }

    return projectedPath(std::move(points), true);
}
