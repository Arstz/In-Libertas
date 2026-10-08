#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QIODevice>
#include <QtMultimedia/QAudioFormat>

#include <memory>

class AudioPlaybackStream final : public QIODevice {
public:
    AudioPlaybackStream(QByteArray samples, const QAudioFormat& format,
        qint64 positionMilliseconds, qint64 durationMilliseconds, qreal playbackRate);
    ~AudioPlaybackStream() override;

    [[nodiscard]] bool isSequential() const override;
    [[nodiscard]] bool atEnd() const override;
    [[nodiscard]] qint64 bytesAvailable() const override;

protected:
    qint64 readData(char* data, qint64 maximumSize) override;
    qint64 writeData(const char* data, qint64 size) override;

private:
    struct ProcessingState;
    std::unique_ptr<ProcessingState> m_processing;
};
