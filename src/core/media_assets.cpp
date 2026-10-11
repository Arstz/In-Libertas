#include "core/media_assets.h"
#include "core/jacket_codec.h"

#include <QtCore/QBuffer>
#include <QtCore/QSaveFile>
#include <QtGui/QColorSpace>
#include <QtGui/QImageReader>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace infalsus {

namespace {

constexpr qint64 kMaximumJacketPixels = 67108864;

struct AudioFileFormat {
    const char* demuxer;
    AVCodecID codec;
    const char* extensions;
};

constexpr AudioFileFormat kAudioFileFormats[]{
    {"wav", AV_CODEC_ID_PCM_S16LE, "wav"},
    {"mp3", AV_CODEC_ID_MP3, "mp3 mp2"},
    {"flac", AV_CODEC_ID_FLAC, "flac"},
    {"ogg", AV_CODEC_ID_VORBIS, "ogg oga"},
    {"ogg", AV_CODEC_ID_OPUS, "opus ogg oga"},
    {"aac", AV_CODEC_ID_AAC, "aac"},
    {"mov", AV_CODEC_ID_AAC, "m4a mp4"},
    {"mov", AV_CODEC_ID_ALAC, "m4a mp4"},
    {"aiff", AV_CODEC_ID_PCM_S16BE, "aif aiff aifc"},
    {"asf", AV_CODEC_ID_WMAV2, "wma asf"},
    {"wv", AV_CODEC_ID_WAVPACK, "wv"},
    {"ape", AV_CODEC_ID_APE, "ape"},
    {"au", AV_CODEC_ID_PCM_MULAW, "au snd"},
    {"caf", AV_CODEC_ID_PCM_S16LE, "caf"},
    {"ac3", AV_CODEC_ID_AC3, "ac3"},
    {"eac3", AV_CODEC_ID_EAC3, "eac3"},
};

[[nodiscard]] QString fileDialogFilter(const QString& label, const QStringList& filters) {
    return QStringLiteral("%1 (%2);;All files (*)").arg(label, filters.join(QLatin1Char(' ')));
}

[[nodiscard]] QImage normalizedJacket(const QImage& image) {
    QImage normalized;
    if (image.colorSpace().isValid()) {
        normalized = image.convertedToColorSpace(QColorSpace::SRgb, QImage::Format_RGBA8888);
    } else {
        normalized = image.convertToFormat(QImage::Format_RGBA8888);
    }
    if (normalized.width() > 2048 || normalized.height() > 2048) {
        normalized = normalized.convertToFormat(QImage::Format_ARGB32_Premultiplied)
            .scaled(2048, 2048, Qt::KeepAspectRatio, Qt::SmoothTransformation)
            .convertToFormat(QImage::Format_RGBA8888);
    }
    return normalized;
}

[[nodiscard]] bool readJacket(QImageReader& reader, QImage* image, QString* error) {
    reader.setAutoTransform(true);
    const QSize size = reader.size();
    if (size.isValid() && static_cast<qint64>(size.width()) * size.height() > kMaximumJacketPixels) {
        *error = QStringLiteral("The jacket dimensions are too large.");
        return false;
    }
    const QImage decoded = reader.read();
    if (decoded.isNull()) {
        *error = QStringLiteral("The jacket cannot be decoded: %1").arg(reader.errorString());
        return false;
    }
    if (static_cast<qint64>(decoded.width()) * decoded.height() > kMaximumJacketPixels) {
        *error = QStringLiteral("The jacket dimensions are too large.");
        return false;
    }
    if (image != nullptr) {
        *image = normalizedJacket(decoded);
        if (image->isNull()) {
            *error = QStringLiteral("The jacket could not be normalized.");
            return false;
        }
    }

    return true;
}

} // namespace

QStringList audioNameFilters() {
    QStringList filters;
    for (const AudioFileFormat& format : kAudioFileFormats) {
        if (av_find_input_format(format.demuxer) == nullptr || avcodec_find_decoder(format.codec) == nullptr) {
            continue;
        }
        for (const QString& extension : QString::fromLatin1(format.extensions).split(QLatin1Char(' '))) {
            filters.append(QStringLiteral("*.%1").arg(extension));
        }
    }
    filters.removeDuplicates();
    filters.sort();

    return filters;
}

QString audioFileDialogFilter() {
    return fileDialogFilter(QStringLiteral("Audio files"), audioNameFilters());
}

QStringList imageNameFilters() {
    QStringList filters;
    for (const QByteArray& format : QImageReader::supportedImageFormats()) {
        filters.append(QStringLiteral("*.%1").arg(QString::fromLatin1(format)));
    }
    filters.removeDuplicates();
    filters.sort();

    return filters;
}

QString imageFileDialogFilter() {
    return fileDialogFilter(QStringLiteral("Images"), imageNameFilters());
}

bool readJacketImage(const QString& path, QImage* image, QString* error) {
    QImageReader reader(path);

    return readJacket(reader, image, error);
}

bool readJacketImage(const QByteArray& data, QImage* image, QString* error) {
    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);
    QImageReader reader(&buffer);

    return readJacket(reader, image, error);
}

QByteArray encodeJacketImage(const QImage& image, QString* error) {
    const QImage normalized = normalizedJacket(image);
    std::size_t size = 0;
    auto* encoded = JacketEncode(normalized.constBits(), normalized.width(), normalized.height(),
        normalized.bytesPerLine(), &size);
    if (encoded == nullptr) {
        *error = QStringLiteral("Could not encode the lossless WebP jacket.");
        return {};
    }
    const QByteArray data(reinterpret_cast<const char*>(encoded), static_cast<qsizetype>(size));
    JacketFree(encoded);
    return data;
}

bool writeJacketImage(const QImage& image, const QString& path, QString* error) {
    QSaveFile file(path);
    const QByteArray data = encodeJacketImage(image, error);
    if (data.isEmpty() || !file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not write the jacket WebP: %1").arg(file.errorString());
        return false;
    }
    if (file.write(data) != data.size() || !file.commit()) {
        *error = QStringLiteral("Could not write the jacket WebP: %1").arg(file.errorString());
        return false;
    }

    return true;
}

} // namespace infalsus
