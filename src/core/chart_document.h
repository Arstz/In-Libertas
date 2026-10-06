#pragma once

#include "core/chart_types.h"
#include "core/chart_project.h"

#include <QtCore/QString>

class ChartDocument {
public:
    struct LoadResult {
        ChartData chart;
        QVector<infalsus::TimingPoint> timingPoints;
        QVector<infalsus::LaneEvent> laneEvents;
        QVector<infalsus::SpeedEvent> speedEvents;
        QString error;

        [[nodiscard]] bool succeeded() const;
    };

    [[nodiscard]] static LoadResult load(const QString& filePath);
    [[nodiscard]] static bool save(const QString& filePath, const ChartData& chart,
        const QVector<infalsus::TimingPoint>& timingPoints, const QVector<infalsus::LaneEvent>& laneEvents,
        const QVector<infalsus::SpeedEvent>& speedEvents, QString* error);
};
