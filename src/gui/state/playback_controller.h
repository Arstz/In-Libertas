#pragma once

#include "gui/state/playback_rates.h"

#include <QtCore/QBuffer>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtMultimedia/QAudioDecoder>
#include <QtMultimedia/QAudioSink>

#include <chrono>
#include <memory>

class AudioPlaybackStream;

class PlaybackController final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(QObject* parent = nullptr);
    ~PlaybackController() override;

    [[nodiscard]] qint64 position() const;
    [[nodiscard]] bool isPlaying() const;
    [[nodiscard]] qreal playbackRate() const;
    [[nodiscard]] qreal volume() const;

    void setChartDuration(qint64 durationMilliseconds);
    void setAudioSource(const QUrl& source);
    void setAudioData(QByteArray audioData, const QUrl& sourceHint);
    void setPosition(qint64 positionMilliseconds);
    void setPlaybackRate(qreal playbackRate);
    void setVolume(qreal volume);
    void togglePlayback();
    void scrubBy(qint64 offsetMilliseconds);
    void increasePlaybackRate();
    void decreasePlaybackRate();

signals:
    void audioError(const QString& message);
    void audioSourceChanged();
    void audioDecoded(const QByteArray& samples, const QAudioFormat& format);
    void mediaDurationChanged(qint64 durationMilliseconds);
    void positionChanged(qint64 positionMilliseconds);
    void playbackChanged(bool isPlaying);
    void playbackRateChanged(qreal playbackRate);

private slots:
    void updateClock();
    void appendDecodedAudio();
    void finishDecoding();

private:
    [[nodiscard]] qint64 clockPosition() const;
    void resetAudioSource();
    void resetAudioPlayback();
    void createAudioDecoder();
    void startAudioPlayback();
    void handleAudioError(const QString& message);
    void changePlaybackRate(int direction);
    void restartClock(qint64 positionMilliseconds);
    void stopClock();
    void publishPosition(qint64 positionMilliseconds);

    QBuffer m_audioBuffer;
    QAudioFormat m_audioFormat;
    std::chrono::steady_clock::time_point m_clockAnchor = std::chrono::steady_clock::now();
    std::unique_ptr<QAudioDecoder> m_audioDecoder;
    std::unique_ptr<AudioPlaybackStream> m_audioStream;
    std::unique_ptr<QAudioSink> m_audioSink;
    QTimer m_clockUpdateTimer;
    QByteArray m_audioData;
    QByteArray m_decodedAudio;
    qint64 m_chartDurationMilliseconds = 0;
    qint64 m_clockAnchorPositionMilliseconds = 0;
    qint64 m_audioStartPositionMilliseconds = 0;
    qint64 m_positionMilliseconds = 0;
    qreal m_playbackRate = infalsus::gui::kFullPlaybackRate;
    qreal m_volume = 1.0;
    bool m_playbackActive = false;
    bool m_hasAudioSource = false;
    bool m_usingFallbackClock = false;
    bool m_decodingAudio = false;
    bool m_audioFailed = false;
};
