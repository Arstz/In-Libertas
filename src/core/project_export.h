#pragma once

#include "core/chart_project.h"

#include <QtCore/QJsonObject>

namespace infalsus {

inline constexpr int kProjectConverterRevision = 1;
inline constexpr char kProjectOwnershipFile[] = ".inlibertas-export.json";

[[nodiscard]] QString projectExportError(const ChartProject& project);
[[nodiscard]] QJsonObject projectExportConfig(const ChartProject& project);
[[nodiscard]] bool writeProjectExport(const QString& directoryPath, const ChartProject& project,
    const QByteArray& audio, const QByteArray& jacket, QString* error);

} // namespace infalsus
