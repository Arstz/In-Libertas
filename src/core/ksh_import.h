#pragma once

#include "core/chart_project.h"

namespace infalsus {

inline constexpr double kMinimumKshFieldWidthLanes = 1.0;
inline constexpr double kDefaultKshFieldWidthLanes = kMinimumKshFieldWidthLanes;
inline constexpr double kMaximumKshFieldWidthLanes = 6.0;

enum class KshLaser { Blue, Red };

struct KshImportOptions {
    KshLaser flickPriority = KshLaser::Blue;
    double fieldWidthLanes = kDefaultKshFieldWidthLanes;
};

struct KshImportResult {
    ChartMetadata metadata;
    DifficultyChart chart;
    QString songPath;
    QString jacketPath;
    QString error;
};

[[nodiscard]] KshImportResult importKsh(const QString& filePath, const KshImportOptions& options = {});

} // namespace infalsus
