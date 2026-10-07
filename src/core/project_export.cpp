#include "core/project_export.h"

#include "core/chart_document.h"
#include "core/media_assets.h"
#include "core/project_format.h"

#include <QtCore/QCryptographicHash>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QSaveFile>

#include <cmath>

namespace infalsus {

namespace {

[[nodiscard]] bool writeBytes(const QString& path, const QByteArray& data, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        *error = QStringLiteral("Could not write %1: %2").arg(path, file.errorString());
        return false;
    }

    return true;
}

[[nodiscard]] QJsonValue optionalValue(const QString& value) {
    return value.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(value);
}

[[nodiscard]] QByteArray smallJacketData(const QByteArray& jacket, QString* error) {
    QImage image;
    if (!readJacketImage(jacket, &image, error)) {
        return {};
    }

    return encodeJacketImage(image.convertToFormat(QImage::Format_ARGB32_Premultiplied).scaled(
        kProjectSmallJacketExtent, kProjectSmallJacketExtent, Qt::IgnoreAspectRatio, Qt::SmoothTransformation), error);
}

} // namespace

QString projectExportError(const ChartProject& project) {
    if (!isValidProjectChartId(project.chartId)) {
        return QStringLiteral("Enter a filename-safe Chart ID before exporting.");
    }
    if (!std::isfinite(project.metadata.previewStartSeconds) || !std::isfinite(project.metadata.previewEndSeconds)
        || project.metadata.previewStartSeconds < 0 || project.metadata.previewStartSeconds >= project.metadata.previewEndSeconds) {
        return QStringLiteral("Preview start time must be nonnegative and earlier than preview end time.");
    }
    for (const DifficultyChart& difficulty : project.difficulties) {
        if (!difficulty.hitObjects.notes.isEmpty()) {
            return {};
        }
    }

    return QStringLiteral("Add at least one game object to a difficulty before exporting.");
}

QJsonObject projectExportConfig(const ChartProject& project) {
    QJsonArray difficulties;
    for (int index = 0; index < 4; ++index) {
        const DifficultyChart& chart = project.difficulties.at(index);
        difficulties.append(chart.hitObjects.notes.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{
            {QStringLiteral("externalChartId"), project.chartId + QString::number(index)},
            {QStringLiteral("rating"), chart.metadata.rating},
            {QStringLiteral("levelSectionIndicator"), QString::number(chart.metadata.rating)},
        }));
    }

    return {
        {QStringLiteral("enabled"), true}, {QStringLiteral("templateSongId"), 2},
        {QStringLiteral("chartId"), project.chartId}, {QStringLiteral("baseName"), project.chartId},
        {QStringLiteral("audioFile"), QStringLiteral("audio.ogg")},
        {QStringLiteral("jacketLargeFile"), QStringLiteral("jacketLarge.png")},
        {QStringLiteral("jacketSmallFile"), QStringLiteral("jacketSmall.png")},
        {QStringLiteral("enableLoosePngJackets"), true},
        {QStringLiteral("songTitle"), project.metadata.songName}, {QStringLiteral("artist"), project.metadata.artistName},
        {QStringLiteral("chartDesigner"), project.metadata.chartDesigner},
        {QStringLiteral("jacketDesigner"), project.metadata.jacketDesigner},
        {QStringLiteral("previewStartSeconds"), project.metadata.previewStartSeconds},
        {QStringLiteral("previewEndSeconds"), project.metadata.previewEndSeconds},
        {QStringLiteral("characterIdentifier"), optionalValue(project.metadata.characterIdentifier)},
        {QStringLiteral("gameplayBackground"), optionalValue(project.metadata.gameplayBackground)},
        {QStringLiteral("difficulties"), difficulties},
    };
}

bool writeProjectExport(const QString& directoryPath, const ChartProject& project,
    const QByteArray& audio, const QByteArray& jacket, QString* error) {
    const QDir directory(directoryPath);
    QJsonObject inventory;
    QByteArray smallJacket;
    QStringList names{QStringLiteral("audio.ogg"), QStringLiteral("jacketLarge.png"),
        QStringLiteral("jacketSmall.png"), QStringLiteral("config.json")};
    *error = projectExportError(project);
    if (!error->isEmpty() || !validateProjectMedia(audio, jacket, error)) {
        return false;
    }
    smallJacket = smallJacketData(jacket, error);
    if (smallJacket.isEmpty()) {
        return false;
    }
    if (!QDir().mkpath(directoryPath)) {
        *error = QStringLiteral("Could not create the export folder.");
        return false;
    }
    if (!writeBytes(directory.filePath(names[0]), audio, error)
        || !writeBytes(directory.filePath(names[1]), jacket, error)
        || !writeBytes(directory.filePath(names[2]), smallJacket, error)) {
        return false;
    }
    for (int index = 0; index < 4; ++index) {
        const DifficultyChart& chart = project.difficulties.at(index);
        if (chart.hitObjects.notes.isEmpty()) {
            continue;
        }
        const QString name = project.chartId + QString::number(index) + QStringLiteral(".spc");
        if (!ChartDocument::save(directory.filePath(name), chart.hitObjects,
            chart.timingPoints, chart.laneEvents, chart.speedEvents, error)) {
            return false;
        }
        names.append(name);
    }
    if (!writeBytes(directory.filePath(QStringLiteral("config.json")),
        QJsonDocument(projectExportConfig(project)).toJson(QJsonDocument::Indented), error)) {
        return false;
    }
    for (const QString& name : names) {
        QFile file(directory.filePath(name));
        QCryptographicHash hash(QCryptographicHash::Sha256);
        if (!file.open(QIODevice::ReadOnly) || !hash.addData(&file)) {
            *error = QStringLiteral("Could not verify the exported file %1.").arg(name);
            return false;
        }
        inventory.insert(name, QString::fromLatin1(hash.result().toHex()));
    }

    return writeBytes(directory.filePath(QString::fromLatin1(kProjectOwnershipFile)), QJsonDocument(QJsonObject{
        {QStringLiteral("version"), 1}, {QStringLiteral("chartId"), project.chartId},
        {QStringLiteral("converterRevision"), kProjectConverterRevision},
        {QStringLiteral("sourceSha256"), QJsonValue(QJsonValue::Null)}, {QStringLiteral("files"), inventory},
    }).toJson(QJsonDocument::Indented), error);
}

} // namespace infalsus
