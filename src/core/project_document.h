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
        QByteArray sourceSha256;
        QString songFileName;
        QString jacketFileName;
        QString error;
        quint16 sourceVersion = 0;

        [[nodiscard]] bool succeeded() const;
    };

    [[nodiscard]] static bool save(const QString& filePath, const ChartProject& project, QString* error);
    [[nodiscard]] static bool save(const QString& filePath, const ChartProject& project,
        const QByteArray& songData, const QByteArray& jacketData, QString* error);
    [[nodiscard]] static LoadResult load(const QString& filePath);
    [[nodiscard]] static bool migrate(const QString& filePath, const LoadResult& normalized,
        QString* backupPath, QString* error);
};

} // namespace infalsus
