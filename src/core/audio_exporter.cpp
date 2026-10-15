#include "core/audio_exporter.h"
#include "core/project_format.h"

#include <QtCore/QBuffer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtMultimedia/QAudioBuffer>
#include <QtMultimedia/QAudioDecoder>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <limits>

namespace infalsus {

namespace {

constexpr int kAudioProbeTimeoutMilliseconds = 10000;
constexpr int kExportIdleTimeoutMilliseconds = 30000;
constexpr int kProgressIntervalMilliseconds = 100;
constexpr int kVorbisQuality = 7;
constexpr int kExportChannelCount = 2;
constexpr int kExportSampleRate = 48000;
constexpr int kOggHeaderSize = 27;
constexpr int kOggSegmentCountOffset = 26;
constexpr int kOggVersionOffset = 4;
constexpr int kVorbisSignatureSize = 7;
constexpr int kOggProbeSize = 512;
constexpr int kAudioCopyChunkSize = 1024 * 1024;
constexpr double kGamePeakAmplitude = 1.1405839342071802;
constexpr double kPeakToleranceDecibels = 0.1;
constexpr int kMaximumNormalizationAttempts = 4;

[[nodiscard]] bool reportEncodingError(QString* error, const QString& operation, const int code) {
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(code, message, sizeof(message));
    *error = QStringLiteral("Ogg Vorbis encoding failed: %1: %2").arg(operation, QString::fromUtf8(message));
    return false;
}

class VorbisEncoder final {
public:
    explicit VorbisEncoder(QIODevice& destination) : m_destination(destination) {}

    ~VorbisEncoder() {
        av_packet_free(&m_packet);
        av_frame_free(&m_frame);
        avcodec_free_context(&m_codec);
        avformat_free_context(m_format);
        if (m_io != nullptr) {
            av_freep(&m_io->buffer);
            avio_context_free(&m_io);
        }
    }

    VorbisEncoder(const VorbisEncoder&) = delete;
    VorbisEncoder& operator=(const VorbisEncoder&) = delete;

    [[nodiscard]] bool initialize(QString* error) {
        const AVCodec* encoder = avcodec_find_encoder_by_name("libvorbis");
        if (encoder == nullptr) {
            *error = QStringLiteral("Ogg Vorbis encoding is unavailable. Build and deploy FFmpeg with its libvorbis encoder enabled.");
            return false;
        }
        int result = avformat_alloc_output_context2(&m_format, nullptr, "ogg", nullptr);
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("create Ogg stream"), result);
        }
        m_codec = avcodec_alloc_context3(encoder);
        m_frame = av_frame_alloc();
        m_packet = av_packet_alloc();
        m_stream = avformat_new_stream(m_format, nullptr);
        if (m_codec == nullptr || m_frame == nullptr || m_packet == nullptr || m_stream == nullptr) {
            return reportEncodingError(error, QStringLiteral("allocate encoder"), AVERROR(ENOMEM));
        }
        m_codec->sample_rate = kExportSampleRate;
        m_codec->sample_fmt = AV_SAMPLE_FMT_FLTP;
        av_channel_layout_default(&m_codec->ch_layout, kExportChannelCount);
        m_codec->time_base = AVRational{1, kExportSampleRate};
        m_codec->flags |= AV_CODEC_FLAG_QSCALE;
        m_codec->global_quality = kVorbisQuality * FF_QP2LAMBDA;
        if (m_format->oformat->flags & AVFMT_GLOBALHEADER) {
            m_codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
        result = avcodec_open2(m_codec, encoder, nullptr);
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("open q7 encoder"), result);
        }
        m_stream->time_base = m_codec->time_base;
        result = avcodec_parameters_from_context(m_stream->codecpar, m_codec);
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("set stream parameters"), result);
        }
        m_frame->format = m_codec->sample_fmt;
        m_frame->sample_rate = m_codec->sample_rate;
        m_frame->nb_samples = m_codec->frame_size;
        result = av_channel_layout_copy(&m_frame->ch_layout, &m_codec->ch_layout);
        if (result >= 0) {
            result = av_frame_get_buffer(m_frame, 0);
        }
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("allocate audio frame"), result);
        }

        constexpr int kIoBufferBytes = 32768;
        auto* buffer = static_cast<unsigned char*>(av_malloc(kIoBufferBytes));
        if (buffer == nullptr) {
            return reportEncodingError(error, QStringLiteral("allocate output buffer"), AVERROR(ENOMEM));
        }
        m_io = avio_alloc_context(buffer, kIoBufferBytes, 1, &m_destination, nullptr, &writePacket, &seek);
        if (m_io == nullptr) {
            av_free(buffer);
            return reportEncodingError(error, QStringLiteral("create output device"), AVERROR(ENOMEM));
        }
        m_format->pb = m_io;
        m_format->flags |= AVFMT_FLAG_CUSTOM_IO;
        result = avformat_write_header(m_format, nullptr);
        return result >= 0 || reportEncodingError(error, QStringLiteral("write Ogg header"), result);
    }

    [[nodiscard]] bool append(const QAudioBuffer& buffer, QString* error, const double gain) {
        const float* samples = buffer.constData<float>();
        for (qsizetype offset = 0; offset < buffer.frameCount();) {
            if (m_pendingSamples == 0) {
                m_frame->nb_samples = m_codec->frame_size;
                const int result = av_frame_make_writable(m_frame);
                if (result < 0) {
                    return reportEncodingError(error, QStringLiteral("prepare audio frame"), result);
                }
            }
            const int count = static_cast<int>(std::min<qsizetype>(
                m_codec->frame_size - m_pendingSamples, buffer.frameCount() - offset));
            for (int channel = 0; channel < kExportChannelCount; ++channel) {
                auto* output = reinterpret_cast<float*>(m_frame->data[channel]) + m_pendingSamples;
                for (int sample = 0; sample < count; ++sample) {
                    output[sample] = static_cast<float>(samples[(offset + sample) * kExportChannelCount + channel] * gain);
                }
            }
            offset += count;
            m_pendingSamples += count;
            if (m_pendingSamples == m_codec->frame_size && !submitFrame(error)) {
                return false;
            }
        }
        return true;
    }

    [[nodiscard]] bool finish(QString* error) {
        if (m_pendingSamples > 0 && !submitFrame(error)) {
            return false;
        }
        int result = avcodec_send_frame(m_codec, nullptr);
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("flush audio encoder"), result);
        }
        if (!writePackets(error)) {
            return false;
        }
        result = av_write_trailer(m_format);
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("finish Ogg stream"), result);
        }
        avio_flush(m_io);
        return m_io->error >= 0 || reportEncodingError(error, QStringLiteral("flush output"), m_io->error);
    }

private:
    static int writePacket(void* opaque, const uint8_t* bytes, const int size) {
        auto* device = static_cast<QIODevice*>(opaque);
        return device->write(reinterpret_cast<const char*>(bytes), size) == size ? size : AVERROR(EIO);
    }

    static int64_t seek(void* opaque, const int64_t offset, int whence) {
        auto* device = static_cast<QIODevice*>(opaque);
        whence &= ~AVSEEK_FORCE;
        if (whence == AVSEEK_SIZE) {
            return device->size();
        }
        qint64 position = offset;
        if (whence == SEEK_CUR) {
            position += device->pos();
        } else if (whence == SEEK_END) {
            position += device->size();
        } else if (whence != SEEK_SET) {
            return AVERROR(EINVAL);
        }
        return device->seek(position) ? position : AVERROR(EIO);
    }

    [[nodiscard]] bool submitFrame(QString* error) {
        m_frame->nb_samples = m_pendingSamples;
        m_frame->pts = m_encodedSamples;
        const int result = avcodec_send_frame(m_codec, m_frame);
        if (result < 0) {
            return reportEncodingError(error, QStringLiteral("encode audio frame"), result);
        }
        m_encodedSamples += m_pendingSamples;
        m_pendingSamples = 0;
        return writePackets(error);
    }

    [[nodiscard]] bool writePackets(QString* error) {
        for (;;) {
            int result = avcodec_receive_packet(m_codec, m_packet);
            if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                return true;
            }
            if (result < 0) {
                return reportEncodingError(error, QStringLiteral("receive audio packet"), result);
            }
            av_packet_rescale_ts(m_packet, m_codec->time_base, m_stream->time_base);
            m_packet->stream_index = m_stream->index;
            result = av_interleaved_write_frame(m_format, m_packet);
            av_packet_unref(m_packet);
            if (result < 0) {
                return reportEncodingError(error, QStringLiteral("write audio packet"), result);
            }
        }
    }

    QIODevice& m_destination;
    AVFormatContext* m_format = nullptr;
    AVCodecContext* m_codec = nullptr;
    AVIOContext* m_io = nullptr;
    AVStream* m_stream = nullptr;
    AVFrame* m_frame = nullptr;
    AVPacket* m_packet = nullptr;
    int m_pendingSamples = 0;
    qint64 m_encodedSamples = 0;
};

[[nodiscard]] bool isOggVorbis(const QByteArray& header) {
    if (header.size() < kOggHeaderSize || !header.startsWith("OggS") || header.at(kOggVersionOffset) != 0) {
        return false;
    }
    const int packetOffset = kOggHeaderSize + static_cast<quint8>(header.at(kOggSegmentCountOffset));

    return header.mid(packetOffset, kVorbisSignatureSize) == QByteArray("\x01vorbis", kVorbisSignatureSize);
}

[[nodiscard]] bool copyVorbisAudio(QIODevice& source, QIODevice& destination, QString* error,
    const AudioExportProgress& progress) {
    while (!source.atEnd()) {
        if (progress && !progress(source.pos(), source.size())) {
            *error = QStringLiteral("Audio export cancelled.");
            return false;
        }
        const QByteArray chunk = source.read(kAudioCopyChunkSize);
        if ((chunk.isEmpty() && !source.atEnd()) || destination.write(chunk) != chunk.size()) {
            *error = QStringLiteral("Could not copy the Ogg Vorbis audio.");
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool decodeAudio(QIODevice& source, QString* error,
    const AudioExportProgress& progress, const std::function<bool(const QAudioBuffer&)>& consumeBuffer) {
    QEventLoop loop;
    QAudioDecoder decoder;
    QTimer idleTimeout;
    QTimer progressTimer;
    bool completed = false;
    bool consuming = false;
    bool decoderFinished = false;
    qint64 decodedFrames = 0;

    const auto fail = [&](const QString& message) {
        if (completed) {
            return;
        }
        *error = message;
        completed = true;
        loop.quit();
    };
    const auto consume = [&] {
        if (consuming || completed) {
            return;
        }
        const QScopedValueRollback guard(consuming, true);
        while (!completed && decoder.bufferAvailable()) {
            const QAudioBuffer buffer = decoder.read();
            if (!buffer.isValid()) {
                continue;
            }
            const QAudioFormat format = buffer.format();
            if (format.sampleFormat() != QAudioFormat::Float || format.sampleRate() != kExportSampleRate
                || format.channelCount() != kExportChannelCount) {
                fail(QStringLiteral("The decoder did not provide stereo 48 kHz float audio."));
                return;
            }
            idleTimeout.start();
            if (progress && !progress(decoder.position(), decoder.duration())) {
                fail(QStringLiteral("Audio export cancelled."));
                return;
            }
            if (completed) {
                return;
            }
            if (!consumeBuffer(buffer)) {
                fail(*error);
                return;
            }
            decodedFrames += buffer.frameCount();
        }
        if (!decoderFinished || completed) {
            return;
        }
        if (decodedFrames == 0) {
            fail(QStringLiteral("The song contains no decodable audio."));
            return;
        }
        completed = true;
        loop.quit();
    };

    idleTimeout.setSingleShot(true);
    idleTimeout.setInterval(kExportIdleTimeoutMilliseconds);
    progressTimer.setInterval(kProgressIntervalMilliseconds);
    QObject::connect(&idleTimeout, &QTimer::timeout, &loop, [&] {
        fail(QStringLiteral("Audio export timed out while waiting for the decoder."));
    });
    QObject::connect(&progressTimer, &QTimer::timeout, &loop, [&] {
        if (!completed && !consuming && progress && !progress(decoder.position(), decoder.duration())) {
            fail(QStringLiteral("Audio export cancelled."));
        }
    });
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop,
        [&](QAudioDecoder::Error) { fail(QStringLiteral("The song cannot be decoded: %1").arg(decoder.errorString())); });
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, consume);
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&] {
        decoderFinished = true;
        consume();
    });

    QAudioFormat format;
    format.setSampleRate(kExportSampleRate);
    format.setChannelCount(kExportChannelCount);
    format.setSampleFormat(QAudioFormat::Float);
    decoder.setAudioFormat(format);
    decoder.setSourceDevice(&source);
    idleTimeout.start();
    progressTimer.start();
    decoder.start();
    if (!completed) {
        loop.exec();
    }
    progressTimer.stop();
    idleTimeout.stop();
    decoder.stop();
    return error->isEmpty();
}

[[nodiscard]] bool transcodeAudio(QIODevice& source, QIODevice& destination, QString* error,
    const AudioExportProgress& progress, const double gain = 1.0) {
    VorbisEncoder encoder(destination);
    return encoder.initialize(error)
        && decodeAudio(source, error, progress, [&](const QAudioBuffer& buffer) {
            return encoder.append(buffer, error, gain);
        })
        && encoder.finish(error);
}

[[nodiscard]] bool measureAudioPeak(QIODevice& source, double* peak, QString* error,
    const AudioExportProgress& progress) {
    *peak = 0.0;
    return decodeAudio(source, error, progress, [&](const QAudioBuffer& buffer) {
        const float* samples = buffer.constData<float>();
        for (qsizetype index = 0; index < buffer.sampleCount(); ++index) {
            if (!std::isfinite(samples[index])) {
                *error = QStringLiteral("The song contains non-finite audio samples.");
                return false;
            }
            *peak = std::max(*peak, std::abs(static_cast<double>(samples[index])));
        }
        return true;
    });
}

[[nodiscard]] bool normalizeAudioPeak(QIODevice& source, QIODevice& destination, QString* error,
    const AudioExportProgress& progress) {
    constexpr qint64 kProgressStages = 2 + kMaximumNormalizationAttempts * 2;
    qint64 reportedPosition = 0;
    const auto stageProgress = [&](const int stage) -> AudioExportProgress {
        return [&, stage](const qint64 position, const qint64 duration) {
            const qint64 fraction = duration > 0
                ? static_cast<qint64>(1000.0 * std::clamp(static_cast<double>(position) / duration, 0.0, 1.0)) : 0;
            reportedPosition = std::max(reportedPosition, stage * qint64(1000) + fraction);
            return !progress || progress(reportedPosition, kProgressStages * 1000);
        };
    };

    double sourcePeak = 0.0;
    if (!measureAudioPeak(source, &sourcePeak, error, stageProgress(0))) {
        return false;
    }
    double gain = sourcePeak > 0.0 ? kGamePeakAmplitude / sourcePeak : 1.0;
    double bestError = std::numeric_limits<double>::infinity();
    QByteArray bestAudio;
    for (int attempt = 0; attempt < kMaximumNormalizationAttempts; ++attempt) {
        if (!source.seek(0)) {
            *error = QStringLiteral("Could not rewind the project audio.");
            return false;
        }
        QByteArray bytes;
        QBuffer encoded(&bytes);
        encoded.open(QIODevice::ReadWrite);
        if (!transcodeAudio(source, encoded, error, stageProgress(1 + attempt * 2), gain)) {
            return false;
        }
        if (sourcePeak == 0.0) {
            bestAudio = std::move(bytes);
            break;
        }
        encoded.seek(0);
        double encodedPeak = 0.0;
        if (!measureAudioPeak(encoded, &encodedPeak, error, stageProgress(2 + attempt * 2))) {
            return false;
        }
        if (encodedPeak == 0.0) {
            *error = QStringLiteral("The normalized song contains no audible samples.");
            return false;
        }
        const double peakError = std::abs(20.0 * std::log10(encodedPeak / kGamePeakAmplitude));
        if (peakError < bestError) {
            bestAudio = std::move(bytes);
            bestError = peakError;
        }
        if (peakError <= kPeakToleranceDecibels) {
            break;
        }
        gain *= kGamePeakAmplitude / encodedPeak;
    }
    QBuffer result(&bestAudio);
    result.open(QIODevice::ReadOnly);
    return copyVorbisAudio(result, destination, error, progress ? stageProgress(kProgressStages - 1) : AudioExportProgress{});
}

[[nodiscard]] bool validateAudio(const QString& path, QIODevice* source, QString* error) {
    QEventLoop loop;
    QAudioDecoder decoder;
    QTimer timeout;
    bool completed = false;
    bool valid = false;
    error->clear();

    const auto finish = [&] {
        completed = true;
        loop.quit();
    };
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, [&] {
        *error = QStringLiteral("Timed out while checking the selected audio.");
        finish();
    });
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, [&] {
        const QAudioBuffer buffer = decoder.read();
        valid = buffer.isValid() && buffer.frameCount() > 0;
        finish();
    });
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, finish);
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop,
        [&](QAudioDecoder::Error) {
        *error = QStringLiteral("The song cannot be decoded: %1").arg(decoder.errorString());
        finish();
    });
    if (source != nullptr) {
        decoder.setSourceDevice(source);
    } else {
        decoder.setSource(QUrl::fromLocalFile(path));
    }
    timeout.start(kAudioProbeTimeoutMilliseconds);
    decoder.start();
    if (!completed) {
        loop.exec(QEventLoop::ExcludeUserInputEvents);
    }
    decoder.stop();
    if (!valid && error->isEmpty()) {
        *error = QStringLiteral("The song contains no decodable audio.");
    }

    return valid && error->isEmpty();
}

} // namespace

bool validateAudioSource(const QString& path, QString* error) {
    return validateAudio(path, nullptr, error);
}

namespace {

[[nodiscard]] bool writeOggAudio(const QString& sourcePath, const QByteArray& sourceData,
    QIODevice& destination, QString* error, const AudioExportProgress& progress,
    const bool forceTranscode = false, const bool normalizePeak = false) {
    QFile file(sourcePath);
    QBuffer buffer;
    QIODevice* source = sourceData.isEmpty() ? static_cast<QIODevice*>(&file) : &buffer;
    error->clear();
    buffer.setData(sourceData);
    if (progress && !progress(0, 0)) {
        *error = QStringLiteral("Audio export cancelled.");
        return false;
    }
    if (!source->open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not read the project song: %1").arg(source->errorString());
        return false;
    }
    const QByteArray header = source->peek(kOggProbeSize);
    if (!forceTranscode && !normalizePeak && isOggVorbis(header) && isProjectVorbisAudio(source->peek(source->size()))) {
        if (!validateAudio({}, source, error)) {
            return false;
        }
        if (!source->seek(0)) {
            *error = QStringLiteral("Could not rewind the project audio.");
            return false;
        }
        return copyVorbisAudio(*source, destination, error, progress);
    }

    return normalizePeak ? normalizeAudioPeak(*source, destination, error, progress)
                         : transcodeAudio(*source, destination, error, progress);
}

} // namespace

QByteArray encodeOggAudio(const QString& sourcePath, const QByteArray& sourceData,
    QString* error, const AudioExportProgress& progress, const bool normalizePeak) {
    QByteArray bytes;
    QBuffer destination(&bytes);
    destination.open(QIODevice::WriteOnly);
    if (!writeOggAudio(sourcePath, sourceData, destination, error, progress, true, normalizePeak)) {
        return {};
    }

    return bytes;
}

bool exportOggAudio(const QString& sourcePath, const QByteArray& sourceData,
    const QString& destinationPath, QString* error, const AudioExportProgress& progress) {
    QSaveFile destination(destinationPath);
    error->clear();
    if (!destination.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not write the exported song: %1").arg(destination.errorString());
        return false;
    }
    if (!writeOggAudio(sourcePath, sourceData, destination, error, progress)) {
        return false;
    }
    if (!destination.commit()) {
        *error = QStringLiteral("Could not finish writing the Ogg Vorbis audio.");
        return false;
    }

    return true;
}

} // namespace infalsus
