#include "gui/widgets/verification_widget.h"

#include "core/timing_grid.h"
#include "gui/app/game_palette.h"

#include <QtGui/QPalette>
#include <QtCore/QVariant>
#include <QtWidgets/QListWidget>
#include <QtWidgets/QVBoxLayout>

#include <algorithm>
#include <array>
#include <cmath>

namespace infalsus::gui {

namespace {

constexpr int kFloorLaneCount = 6;
constexpr int kFirstCentralLane = 1;
constexpr int kLastCentralLane = 4;
constexpr int kRightOuterLane = 5;

[[nodiscard]] bool isGroundObject(const ChartNote& hitObject) {
    return hitObject.side == FloorSide::Central
        || hitObject.side == FloorSide::LeftOuter
        || hitObject.side == FloorSide::RightOuter;
}

[[nodiscard]] bool hasValidCentralFloorSpan(const ChartNote& hitObject) {
    constexpr double kFirstCentralCoordinate = 0.25;
    constexpr double kLastCentralCoordinate = 1.0;
    constexpr double kSingleLaneWidth = 0.25;
    constexpr double kRightCentralBoundary = 1.25;
    constexpr double kTolerance = 0.0001;
    const auto validEndpoint = [](const double coordinate, const double width) {
        return std::isfinite(coordinate) && std::isfinite(width)
            && coordinate >= kFirstCentralCoordinate - kTolerance
            && coordinate <= kLastCentralCoordinate + kTolerance
            && width >= kSingleLaneWidth - kTolerance
            && coordinate + width <= kRightCentralBoundary + kTolerance;
    };

    return validEndpoint(hitObject.startX, hitObject.startWidth)
        && validEndpoint(hitObject.endX, hitObject.endWidth);
}

[[nodiscard]] std::array<bool, kFloorLaneCount> lanesAt(
    const ChartNote& hitObject, const qint64 timestampMilliseconds) {
    std::array<bool, kFloorLaneCount> lanes{};
    if (!isGroundObject(hitObject)) {
        return lanes;
    }
    if (hitObject.side == FloorSide::LeftOuter) {
        lanes.front() = true;
        return lanes;
    }
    if (hitObject.side == FloorSide::RightOuter) {
        lanes.at(kRightOuterLane) = true;
        return lanes;
    }

    double progress = 0.0;
    if (hitObject.kind == NoteKind::Hold && hitObject.endMilliseconds > hitObject.startMilliseconds) {
        progress = std::clamp(
            static_cast<double>(timestampMilliseconds - hitObject.startMilliseconds)
                / static_cast<double>(hitObject.endMilliseconds - hitObject.startMilliseconds),
            0.0, 1.0);
    }
    const double coordinate = hitObject.startX + (hitObject.endX - hitObject.startX) * progress;
    const double width = hitObject.startWidth + (hitObject.endWidth - hitObject.startWidth) * progress;
    const int firstLane = std::clamp(
        static_cast<int>(std::lround(coordinate * 4.0)), kFirstCentralLane, kLastCentralLane);
    const int laneCount = std::clamp(
        static_cast<int>(std::lround(width * 4.0)), 1, kRightOuterLane - firstLane);
    for (int lane = firstLane; lane < firstLane + laneCount; ++lane) {
        lanes.at(lane) = true;
    }

    return lanes;
}

[[nodiscard]] bool shareLaneAt(const ChartNote& first, const ChartNote& second, const qint64 timestampMilliseconds) {
    const auto firstLanes = lanesAt(first, timestampMilliseconds);
    const auto secondLanes = lanesAt(second, timestampMilliseconds);
    for (int lane = 0; lane < kFloorLaneCount; ++lane) {
        if (firstLanes.at(lane) && secondLanes.at(lane)) {
            return true;
        }
    }

    return false;
}

[[nodiscard]] bool occupiesMultipleLanesAt(const ChartNote& hitObject, const qint64 timestampMilliseconds) {
    const auto lanes = lanesAt(hitObject, timestampMilliseconds);
    return std::count(lanes.cbegin(), lanes.cend(), true) > 1;
}

[[nodiscard]] bool occupiesSingleLaneAt(const ChartNote& hitObject, const qint64 timestampMilliseconds) {
    const auto lanes = lanesAt(hitObject, timestampMilliseconds);
    return std::count(lanes.cbegin(), lanes.cend(), true) == 1;
}

[[nodiscard]] VerificationIssue issueFor(
    const ChartNote& first, const ChartNote& second, const qint64 timestampMilliseconds, const QString& message) {
    QVector<std::uint64_t> hitObjectIds{first.id, second.id};
    std::sort(hitObjectIds.begin(), hitObjectIds.end());
    return {
        .timestampMilliseconds = timestampMilliseconds,
        .hitObjectIds = std::move(hitObjectIds),
        .message = message,
    };
}

[[nodiscard]] QString formatTimestamp(const qint64 timestampMilliseconds) {
    constexpr qint64 kMillisecondsPerSecond = 1000;
    const qint64 clampedTimestamp = std::max<qint64>(0, timestampMilliseconds);
    const qint64 totalSeconds = clampedTimestamp / kMillisecondsPerSecond;
    return QStringLiteral("%1:%2:%3")
        .arg(totalSeconds / 60, 2, 10, QLatin1Char('0'))
        .arg(totalSeconds % 60, 2, 10, QLatin1Char('0'))
        .arg(clampedTimestamp % kMillisecondsPerSecond, 3, 10, QLatin1Char('0'));
}

[[nodiscard]] QString formatIssue(const VerificationIssue& issue) {
    const QString idRange = issue.hitObjectIds.isEmpty()
        ? QStringLiteral("-")
        : QStringLiteral("%1 ... %2")
              .arg(issue.hitObjectIds.constFirst())
              .arg(issue.hitObjectIds.constLast());

    return QStringLiteral("[%1][%2] %3")
        .arg(formatTimestamp(issue.timestampMilliseconds), idRange, issue.message);
}

} // namespace

VerificationWidget::VerificationWidget(QWidget* parent)
    : QWidget(parent) {
    setAutoFillBackground(true);
    QPalette widgetPalette = palette();
    widgetPalette.setColor(QPalette::Window, palette::chrome);
    setPalette(widgetPalette);

    auto* layout = new QVBoxLayout(this);
    m_issues = new QListWidget(this);
    QPalette issuePalette = m_issues->palette();
    issuePalette.setColor(QPalette::Window, palette::chrome);
    issuePalette.setColor(QPalette::Base, palette::chrome);
    issuePalette.setColor(QPalette::AlternateBase, palette::chrome);
    m_issues->setPalette(issuePalette);
    m_issues->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_issues);

    connect(m_issues, &QListWidget::itemDoubleClicked, this, [this](const QListWidgetItem* item) {
        emit timestampRequested(item->data(Qt::UserRole).toLongLong());
    });
}

void VerificationWidget::setChart(const ChartData& chart, const QVector<SpeedEvent>& speedEvents,
    const QVector<TimingPoint>& timingPoints) {
    static constexpr std::array<VerificationRule, 5> kRules{
        &VerificationWidget::verifyDuplicateNotes,
        &VerificationWidget::verifyDuplicateHolds,
        &VerificationWidget::verifyOverlappingMultilane,
        &VerificationWidget::verifyOverlappingHolds,
        &VerificationWidget::verifyCentralFloorSpans,
    };
    QVector<VerificationIssue> issues;
    for (const VerificationRule rule : kRules) {
        issues += rule(chart);
    }
    issues += verifySnappedObjects(chart, timingPoints);
    issues += verifyNegativeSpeedSections(chart, speedEvents);
    std::sort(issues.begin(), issues.end(), [](const VerificationIssue& first, const VerificationIssue& second) {
        if (first.timestampMilliseconds != second.timestampMilliseconds) {
            return first.timestampMilliseconds < second.timestampMilliseconds;
        }
        if (first.hitObjectIds != second.hitObjectIds) {
            return first.hitObjectIds < second.hitObjectIds;
        }
        return first.message < second.message;
    });
    setIssues(std::move(issues));
}

QVector<VerificationIssue> VerificationWidget::verifyDuplicateNotes(const ChartData& chart) {
    QVector<VerificationIssue> issues;
    for (int firstIndex = 0; firstIndex < chart.notes.size(); ++firstIndex) {
        const ChartNote& first = chart.notes.at(firstIndex);
        if (first.kind != NoteKind::Tap || !isGroundObject(first)) {
            continue;
        }
        for (int secondIndex = firstIndex + 1; secondIndex < chart.notes.size(); ++secondIndex) {
            const ChartNote& second = chart.notes.at(secondIndex);
            if (second.kind == NoteKind::Tap && isGroundObject(second)
                && first.startMilliseconds == second.startMilliseconds
                && shareLaneAt(first, second, first.startMilliseconds)) {
                issues.append(issueFor(first, second, first.startMilliseconds, QStringLiteral("Duplicate note")));
            }
        }
    }

    return issues;
}

QVector<VerificationIssue> VerificationWidget::verifyDuplicateHolds(const ChartData& chart) {
    QVector<VerificationIssue> issues;
    for (int firstIndex = 0; firstIndex < chart.notes.size(); ++firstIndex) {
        const ChartNote& first = chart.notes.at(firstIndex);
        if (first.kind != NoteKind::Hold || !isGroundObject(first)) {
            continue;
        }
        for (int secondIndex = firstIndex + 1; secondIndex < chart.notes.size(); ++secondIndex) {
            const ChartNote& second = chart.notes.at(secondIndex);
            if (second.kind == NoteKind::Hold && isGroundObject(second)
                && first.startMilliseconds == second.startMilliseconds
                && shareLaneAt(first, second, first.startMilliseconds)) {
                issues.append(issueFor(first, second, first.startMilliseconds, QStringLiteral("Duplicate hold")));
            }
        }
    }

    return issues;
}

QVector<VerificationIssue> VerificationWidget::verifyOverlappingMultilane(const ChartData& chart) {
    QVector<VerificationIssue> issues;
    for (int multiLaneIndex = 0; multiLaneIndex < chart.notes.size(); ++multiLaneIndex) {
        const ChartNote& multiLane = chart.notes.at(multiLaneIndex);
        if (!isGroundObject(multiLane)
            || !occupiesMultipleLanesAt(multiLane, multiLane.startMilliseconds)) {
            continue;
        }
        for (int singleLaneIndex = 0; singleLaneIndex < chart.notes.size(); ++singleLaneIndex) {
            if (singleLaneIndex == multiLaneIndex) {
                continue;
            }
            const ChartNote& singleLane = chart.notes.at(singleLaneIndex);
            if (!isGroundObject(singleLane)
                || singleLane.startMilliseconds != multiLane.startMilliseconds
                || !occupiesSingleLaneAt(singleLane, singleLane.startMilliseconds)
                || !shareLaneAt(multiLane, singleLane, multiLane.startMilliseconds)) {
                continue;
            }
            issues.append(issueFor(multiLane, singleLane, multiLane.startMilliseconds,
                QStringLiteral("Overlapping multilane")));
        }
    }

    return issues;
}

QVector<VerificationIssue> VerificationWidget::verifyOverlappingHolds(const ChartData& chart) {
    QVector<VerificationIssue> issues;
    for (int holdIndex = 0; holdIndex < chart.notes.size(); ++holdIndex) {
        const ChartNote& hold = chart.notes.at(holdIndex);
        if (hold.kind != NoteKind::Hold || !isGroundObject(hold)
            || hold.endMilliseconds <= hold.startMilliseconds) {
            continue;
        }
        for (int otherIndex = 0; otherIndex < chart.notes.size(); ++otherIndex) {
            if (holdIndex == otherIndex) {
                continue;
            }
            const ChartNote& other = chart.notes.at(otherIndex);
            if (!isGroundObject(other)) {
                continue;
            }
            if (other.kind == NoteKind::Tap
                && other.startMilliseconds >= hold.startMilliseconds
                && other.startMilliseconds < hold.endMilliseconds
                && shareLaneAt(hold, other, other.startMilliseconds)) {
                issues.append(issueFor(hold, other, other.startMilliseconds, QStringLiteral("Overlapping hold")));
            }
            if (other.kind == NoteKind::Hold && holdIndex < otherIndex
                && other.startMilliseconds != hold.startMilliseconds) {
                const qint64 overlapStart = std::max(hold.startMilliseconds, other.startMilliseconds);
                const qint64 overlapEnd = std::min(hold.endMilliseconds, other.endMilliseconds);
                if (overlapStart < overlapEnd && shareLaneAt(hold, other, overlapStart)) {
                    issues.append(issueFor(hold, other, overlapStart, QStringLiteral("Overlapping hold")));
                }
            }
        }
    }

    return issues;
}

QVector<VerificationIssue> VerificationWidget::verifyCentralFloorSpans(const ChartData& chart) {
    QVector<VerificationIssue> issues;
    for (const ChartNote& hitObject : chart.notes) {
        if ((hitObject.kind != NoteKind::Tap && hitObject.kind != NoteKind::Hold)
            || hitObject.side != FloorSide::Central || hasValidCentralFloorSpan(hitObject)) {
            continue;
        }
        issues.append({
            .timestampMilliseconds = hitObject.startMilliseconds,
            .hitObjectIds = {hitObject.id},
            .message = QStringLiteral("Invalid central lane span"),
        });
    }

    return issues;
}

QVector<VerificationIssue> VerificationWidget::verifySnappedObjects(const ChartData& chart,
    const QVector<TimingPoint>& timingPoints) {
    QVector<VerificationIssue> issues;
    if (timingPoints.isEmpty()) {
        return issues;
    }

    for (const ChartNote& hitObject : chart.notes) {
        if (!isValidDivisorTime(timingPoints, hitObject.startMilliseconds)) {
            issues.append({
                .timestampMilliseconds = hitObject.startMilliseconds,
                .hitObjectIds = {hitObject.id},
                .message = QStringLiteral("Object not snapped"),
            });
        }
        if (hitObject.kind != NoteKind::Tap && hitObject.kind != NoteKind::Flick
            && !isValidDivisorTime(timingPoints, hitObject.endMilliseconds)) {
            issues.append({
                .timestampMilliseconds = hitObject.endMilliseconds,
                .hitObjectIds = {hitObject.id},
                .message = QStringLiteral("Object not snapped"),
            });
        }
    }

    return issues;
}

QVector<VerificationIssue> VerificationWidget::verifyNegativeSpeedSections(
    const ChartData& chart, const QVector<SpeedEvent>& speedEvents) {
    QVector<SpeedEvent> orderedEvents = speedEvents;
    std::stable_sort(orderedEvents.begin(), orderedEvents.end(),
        [](const SpeedEvent& first, const SpeedEvent& second) {
            return first.timeMilliseconds < second.timeMilliseconds;
        });

    QVector<VerificationIssue> issues;
    double speed = 1.0;
    qint64 negativeStart = -1;
    const auto addIssue = [&chart, &issues](const qint64 start, const qint64 end) {
        QVector<std::uint64_t> hitObjectIds;
        for (const ChartNote& hitObject : chart.notes) {
            const qint64 objectEnd = std::max(hitObject.startMilliseconds, hitObject.endMilliseconds);
            // A tap belongs to the interval at its timestamp; sustained
            // objects belong whenever any part of their active span overlaps.
            const bool beginsInside = hitObject.startMilliseconds >= start && hitObject.startMilliseconds < end;
            const bool overlapsInside = objectEnd > start && hitObject.startMilliseconds < end;
            if (beginsInside || overlapsInside) {
                hitObjectIds.append(hitObject.id);
            }
        }
        if (hitObjectIds.isEmpty()) {
            return;
        }
        std::sort(hitObjectIds.begin(), hitObjectIds.end());
        issues.append({
            .timestampMilliseconds = start,
            .hitObjectIds = std::move(hitObjectIds),
            .message = QStringLiteral("Negative SV section from [%1] to [%2], hitobjects will not be visible")
                .arg(start).arg(end),
        });
    };

    for (const SpeedEvent& event : orderedEvents) {
        if (speed >= 0.0 && event.speed < 0.0) {
            negativeStart = event.timeMilliseconds;
        } else if (speed < 0.0 && event.speed >= 0.0 && negativeStart >= 0) {
            addIssue(negativeStart, event.timeMilliseconds);
            negativeStart = -1;
        }
        speed = event.speed;
    }
    if (speed < 0.0 && negativeStart >= 0) {
        addIssue(negativeStart, std::max(negativeStart, chart.durationMilliseconds));
    }

    return issues;
}

void VerificationWidget::setIssues(QVector<VerificationIssue> issues) {
    m_issues->clear();
    for (const VerificationIssue& issue : issues) {
        auto* item = new QListWidgetItem(formatIssue(issue), m_issues);
        item->setData(Qt::UserRole, issue.timestampMilliseconds);
    }
}

} // namespace infalsus::gui
