#pragma once

#include "core/audio_exporter.h"

namespace infalsus {

struct NormalizedProjectAssets {
    QByteArray songData;
    QByteArray jacketData;
    QString error;
};

[[nodiscard]] NormalizedProjectAssets normalizeProjectAssets(const QString& songPath, const QByteArray& songData,
    const QString& jacketPath, const QByteArray& jacketData, const AudioExportProgress& progress = {});

} // namespace infalsus
