#pragma once

#include "core/chart_types.h"
#include "core/chart_project.h"

#include <QtCore/QVector>
#include <QtWidgets/QWidget>

#include <cstdint>

class QListWidget;

namespace infalsus::gui {

// Each check produces independent findings, allowing new compatibility rules
// to be added without changing the widget or its navigation behaviour.
struct VerificationIssue {
    qint64 timestampMilliseconds = 0;
    QVector<std::uint64_t> hitObjectIds;
    QString message;
};

class VerificationWidget final : public QWidget {
    Q_OBJECT

public:
    explicit VerificationWidget(QWidget* parent = nullptr);

    void setChart(const ChartData& chart, const QVector<SpeedEvent>& speedEvents,
        const QVector<TimingPoint>& timingPoints);

signals:
    void timestampRequested(qint64 timestampMilliseconds);

private:
    using VerificationRule = QVector<VerificationIssue> (*)(const ChartData& chart);

    static QVector<VerificationIssue> verifyDuplicateNotes(const ChartData& chart);
    static QVector<VerificationIssue> verifyDuplicateHolds(const ChartData& chart);
    static QVector<VerificationIssue> verifyOverlappingMultilane(const ChartData& chart);
    static QVector<VerificationIssue> verifyOverlappingHolds(const ChartData& chart);
    static QVector<VerificationIssue> verifyCentralFloorSpans(const ChartData& chart);
    static QVector<VerificationIssue> verifyZoneGroups(const ChartData& chart);
    static QVector<VerificationIssue> verifySnappedObjects(const ChartData& chart,
        const QVector<TimingPoint>& timingPoints);
    static QVector<VerificationIssue> verifyNegativeSpeedSections(
        const ChartData& chart, const QVector<SpeedEvent>& speedEvents);
    void setIssues(QVector<VerificationIssue> issues);

    QListWidget* m_issues = nullptr;
};

} // namespace infalsus::gui
