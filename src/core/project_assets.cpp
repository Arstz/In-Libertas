#include "core/project_assets.h"

#include "core/media_assets.h"
#include "core/project_format.h"

namespace infalsus {

NormalizedProjectAssets normalizeProjectAssets(const QString& songPath, const QByteArray& songData,
    const QString& jacketPath, const QByteArray& jacketData, const AudioExportProgress& progress) {
    NormalizedProjectAssets result;
    QImage jacket;

    if (!(jacketData.isEmpty() ? readJacketImage(jacketPath, &jacket, &result.error)
        : readJacketImage(jacketData, &jacket, &result.error))) {
        return result;
    }
    result.jacketData = encodeJacketImage(jacket, &result.error);
    if (!result.error.isEmpty()) {
        return result;
    }
    result.songData = encodeOggAudio(songPath, songData, &result.error, progress);
    if (!result.error.isEmpty()) {
        return result;
    }
    static_cast<void>(validateProjectMedia(result.songData, result.jacketData, &result.error));

    return result;
}

} // namespace infalsus
