#pragma once

#include "gui/state/playback_rates.h"

#include <QtCore/QBuffer>
#include <QtCore/QObject>
#include <QtCore/QTimer>
#include <QtCore/QUrl>
#include <QtMultimedia/QAudioOutput>
#include <QtMultimedia/QMediaPlayer>

#include <chrono>
#include <memory>

class PlaybackController final : public QObject {
    Q_OBJECT

public:
    explicit PlaybackController(QObject* parent = nullptr);

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
    void mediaDurationChanged(qint64 durationMilliseconds);
    void positionChanged(qint64 positionMilliseconds);
    void playbackChanged(bool isPlaying);
    void playbackRateChanged(qreal playbackRate);

private slots:
    void updateClock();
    void handleMediaPosition(qint64 positionMilliseconds);
    void handleMediaPlaybackState(QMediaPlayer::PlaybackState state);
    void handleMediaError(QMediaPlayer::Error error, const QString& message);

private:
    [[nodiscard]] qint64 clockPosition() const;
    void resetMediaPlayer();
    void changePlaybackRate(int direction);
    void restartClock(qint64 positionMilliseconds);
    void stopClock();
    void publishPosition(qint64 positionMilliseconds);

    QAudioOutput m_audioOutput;
    QBuffer m_audioBuffer;
    std::chrono::steady_clock::time_point m_clockAnchor;
    std::unique_ptr<QMediaPlayer> m_mediaPlayer;
    QTimer m_clockUpdateTimer;
    QByteArray m_audioData;
    QUrl m_audioSource;
    qint64 m_chartDurationMilliseconds = 0;
    qint64 m_clockAnchorPositionMilliseconds = 0;
    qint64 m_mediaClockStartPositionMilliseconds = 0;
    qint64 m_positionMilliseconds = 0;
    qreal m_playbackRate = infalsus::gui::kFullPlaybackRate;
    bool m_playbackActive = false;
    bool m_hasAudioSource = false;
    bool m_usingFallbackClock = false;
    bool m_waitingForMediaClock = false;
};
