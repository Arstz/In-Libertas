#pragma once

#include "core/chart_project.h"

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace infalsus {

class ProjectDocument final {
public:
    struct LoadResult {
        ChartProject project;
        QByteArray songData;
        QByteArray jacketData;
        QString songFileName;
        QString jacketFileName;
        QString error;

        [[nodiscard]] bool succeeded() const;
    };

    [[nodiscard]] static bool save(const QString& filePath, const ChartProject& project, QString* error);
    [[nodiscard]] static bool save(const QString& filePath, const ChartProject& project,
        const QByteArray& songData, const QByteArray& jacketData, QString* error);
    [[nodiscard]] static LoadResult load(const QString& filePath);
};

} // namespace infalsus
