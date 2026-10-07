#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>

namespace infalsus {

inline constexpr quint16 kProjectVersion = 3;
inline constexpr quint16 kLegacyProjectVersion = 2;
inline constexpr qint64 kMaximumProjectManifestBytes = 32LL * 1024 * 1024;
inline constexpr qint64 kMaximumProjectJacketPixels = 67108864;

[[nodiscard]] bool isValidProjectChartId(const QString& chartId);
[[nodiscard]] bool isProjectVorbisAudio(const QByteArray& audio);
[[nodiscard]] bool validateProjectMedia(const QByteArray& audio, const QByteArray& jacket, QString* error);

} // namespace infalsus
