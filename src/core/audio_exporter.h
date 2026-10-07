#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

#include <functional>

namespace infalsus {

using AudioExportProgress = std::function<bool(qint64 positionMilliseconds, qint64 durationMilliseconds)>;

[[nodiscard]] bool validateAudioSource(const QString& path, QString* error);
[[nodiscard]] QByteArray encodeOggAudio(const QString& sourcePath, const QByteArray& sourceData,
    QString* error, const AudioExportProgress& progress = {});
[[nodiscard]] bool exportOggAudio(const QString& sourcePath, const QByteArray& sourceData,
    const QString& destinationPath, QString* error, const AudioExportProgress& progress = {});

} // namespace infalsus
