#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QStringList>
#include <QtGui/QImage>

namespace infalsus {

[[nodiscard]] QStringList audioNameFilters();
[[nodiscard]] QString audioFileDialogFilter();
[[nodiscard]] QStringList imageNameFilters();
[[nodiscard]] QString imageFileDialogFilter();
[[nodiscard]] bool readJacketImage(const QString& path, QImage* image, QString* error);
[[nodiscard]] bool readJacketImage(const QByteArray& data, QImage* image, QString* error);
[[nodiscard]] QByteArray encodeJacketImage(const QImage& image, QString* error);
[[nodiscard]] bool writeJacketImage(const QImage& image, const QString& path, QString* error);

} // namespace infalsus
