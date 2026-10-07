#include "core/audio_exporter.h"

#include <QtCore/QBuffer>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QSaveFile>
#include <QtCore/QScopedValueRollback>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtMultimedia/QAudioBuffer>
#include <QtMultimedia/QAudioBufferInput>
#include <QtMultimedia/QAudioDecoder>
#include <QtMultimedia/QMediaCaptureSession>
#include <QtMultimedia/QMediaFormat>
#include <QtMultimedia/QMediaRecorder>

namespace infalsus {

namespace {

constexpr int kAudioProbeTimeoutMilliseconds = 10000;
constexpr int kExportIdleTimeoutMilliseconds = 30000;
constexpr int kProgressIntervalMilliseconds = 100;
constexpr int kVorbisBitRate = 320000;
constexpr int kExportChannelCount = 2;
constexpr int kExportSampleRate = 48000;
constexpr int kOggHeaderSize = 27;
constexpr int kOggSegmentCountOffset = 26;
constexpr int kOggVersionOffset = 4;
constexpr int kVorbisSignatureSize = 7;
constexpr int kOggProbeSize = 512;
constexpr int kAudioCopyChunkSize = 1024 * 1024;

[[nodiscard]] bool isOggVorbis(const QByteArray& header) {
    if (header.size() < kOggHeaderSize || !header.startsWith("OggS") || header.at(kOggVersionOffset) != 0) {
        return false;
    }
    const int packetOffset = kOggHeaderSize + static_cast<quint8>(header.at(kOggSegmentCountOffset));

    return header.mid(packetOffset, kVorbisSignatureSize) == QByteArray("\x01vorbis", kVorbisSignatureSize);
}

[[nodiscard]] bool copyVorbisAudio(QIODevice& source, QSaveFile& destination, QString* error,
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
    if (!destination.commit()) {
        *error = QStringLiteral("Could not finish writing the Ogg Vorbis audio.");
        return false;
    }

    return true;
}

[[nodiscard]] bool transcodeAudio(QIODevice& source, QSaveFile& destination, QString* error,
    const AudioExportProgress& progress) {
    QMediaFormat format(QMediaFormat::Ogg);
    format.setAudioCodec(QMediaFormat::AudioCodec::Vorbis);
    if (!format.isSupported(QMediaFormat::Encode)) {
        *error = QStringLiteral("Ogg Vorbis encoding is unavailable. Build and deploy FFmpeg with its libvorbis encoder enabled.");
        return false;
    }

    QEventLoop loop;
    QAudioDecoder decoder;
    QMediaRecorder recorder;
    QAudioBufferInput input;
    QMediaCaptureSession session;
    QAudioBuffer pending;
    QTimer idleTimeout;
    QTimer progressTimer;
    bool completed = false;
    bool decoderFinished = false;
    bool endSent = false;
    bool recordingStarted = false;
    bool pumping = false;
    qint64 decodedFrames = 0;

    const auto fail = [&](const QString& message) {
        if (completed) {
            return;
        }
        *error = message;
        completed = true;
        loop.quit();
    };
    const auto pump = [&] {
        if (completed || endSent || pumping) {
            return;
        }
        QScopedValueRollback<bool> pumpingGuard(pumping, true);
        while (pending.isValid() || decoder.bufferAvailable()) {
            if (!pending.isValid()) {
                pending = decoder.read();
                decodedFrames += pending.frameCount();
                idleTimeout.start();
                if (progress && !progress(decoder.position(), decoder.duration())) {
                    fail(QStringLiteral("Audio export cancelled."));
                    return;
                }
                if (completed) {
                    return;
                }
            }
            if (!recordingStarted) {
                recordingStarted = true;
                recorder.record();
            }
            if (completed || !input.sendAudioBuffer(pending)) {
                return;
            }
            pending = {};
        }
        if (decoderFinished) {
            if (decodedFrames == 0) {
                fail(QStringLiteral("The song contains no decodable audio."));
            } else if (input.sendAudioBuffer({})) {
                endSent = true;
            }
        }
    };

    idleTimeout.setSingleShot(true);
    idleTimeout.setInterval(kExportIdleTimeoutMilliseconds);
    progressTimer.setInterval(kProgressIntervalMilliseconds);
    QObject::connect(&idleTimeout, &QTimer::timeout, &loop, [&] {
        fail(QStringLiteral("Audio export timed out while waiting for the decoder or encoder."));
    });
    QObject::connect(&progressTimer, &QTimer::timeout, &loop, [&] {
        if (progress && !progress(decoder.position(), decoder.duration())) {
            fail(QStringLiteral("Audio export cancelled."));
        }
    });
    QObject::connect(&decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), &loop,
        [&](QAudioDecoder::Error) { fail(QStringLiteral("The song cannot be decoded: %1").arg(decoder.errorString())); });
    QObject::connect(&decoder, &QAudioDecoder::bufferReady, &loop, pump);
    QObject::connect(&decoder, &QAudioDecoder::finished, &loop, [&] {
        decoderFinished = true;
        pump();
    });
    QObject::connect(&input, &QAudioBufferInput::readyToSendAudioBuffer, &loop, pump);
    QObject::connect(&recorder, &QMediaRecorder::errorOccurred, &loop,
        [&](QMediaRecorder::Error, const QString& message) { fail(QStringLiteral("Ogg Vorbis encoding failed: %1").arg(message)); });
    QObject::connect(&recorder, &QMediaRecorder::recorderStateChanged, &loop,
        [&](QMediaRecorder::RecorderState state) {
        if (state != QMediaRecorder::StoppedState || !recordingStarted || completed) {
            return;
        }
        if (!endSent || recorder.error() != QMediaRecorder::NoError
            || recorder.mediaFormat().fileFormat() != QMediaFormat::Ogg
            || recorder.mediaFormat().audioCodec() != QMediaFormat::AudioCodec::Vorbis) {
            fail(QStringLiteral("The encoder did not complete an Ogg Vorbis stream."));
            return;
        }
        completed = true;
        loop.quit();
    });

    session.setAudioBufferInput(&input);
    session.setRecorder(&recorder);
    recorder.setMediaFormat(format);
    recorder.setEncodingMode(QMediaRecorder::AverageBitRateEncoding);
    recorder.setAudioBitRate(kVorbisBitRate);
    recorder.setAudioChannelCount(kExportChannelCount);
    recorder.setAudioSampleRate(kExportSampleRate);
    recorder.setAutoStop(true);
    recorder.setOutputDevice(&destination);
    decoder.setSourceDevice(&source);
    idleTimeout.start();
    progressTimer.start();
    if (!completed) {
        decoder.start();
    }
    if (!completed) {
        loop.exec();
    }
    progressTimer.stop();
    idleTimeout.stop();
    decoder.stop();
    recorder.stop();
    session.setRecorder(nullptr);
    session.setAudioBufferInput(nullptr);
    recorder.setOutputDevice(nullptr);
    if (!error->isEmpty()) {
        return false;
    }
    if (destination.size() == 0 || !destination.commit()) {
        *error = QStringLiteral("Could not finish writing the Ogg Vorbis audio.");
        return false;
    }

    return true;
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

bool exportOggAudio(const QString& sourcePath, const QByteArray& sourceData,
    const QString& destinationPath, QString* error, const AudioExportProgress& progress) {
    QFile file(sourcePath);
    QBuffer buffer;
    QSaveFile destination(destinationPath);
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
    if (!destination.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not write the exported song: %1").arg(destination.errorString());
        return false;
    }
    const QByteArray header = source->peek(kOggProbeSize);
    if (isOggVorbis(header)) {
        if (!validateAudio({}, source, error)) {
            return false;
        }
        if (!source->seek(0)) {
            *error = QStringLiteral("Could not rewind the project audio.");
            return false;
        }
        return copyVorbisAudio(*source, destination, error, progress);
    }

    return transcodeAudio(*source, destination, error, progress);
}

} // namespace infalsus
