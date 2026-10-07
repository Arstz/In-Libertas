#include "gui/state/playback_controller.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr int kClockUpdateIntervalMilliseconds = 4;
using infalsus::gui::kFullPlaybackRate;
using infalsus::gui::kPlaybackRates;

} // namespace

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent) {
    resetMediaPlayer();
    m_clockUpdateTimer.setInterval(kClockUpdateIntervalMilliseconds);
    m_clockUpdateTimer.setTimerType(Qt::PreciseTimer);

    connect(&m_clockUpdateTimer, &QTimer::timeout, this, &PlaybackController::updateClock);
}

qint64 PlaybackController::position() const {
    return m_positionMilliseconds;
}

bool PlaybackController::isPlaying() const {
    return m_playbackActive;
}

qreal PlaybackController::playbackRate() const {
    return m_playbackRate;
}

qreal PlaybackController::volume() const {
    return m_audioOutput.volume();
}

void PlaybackController::setChartDuration(const qint64 durationMilliseconds) {
    const qint64 clampedDuration = std::max<qint64>(durationMilliseconds, 0);
    if (m_chartDurationMilliseconds == clampedDuration) {
        return;
    }
    m_chartDurationMilliseconds = clampedDuration;
    setPosition(m_positionMilliseconds);
}

void PlaybackController::setAudioSource(const QUrl& source) {
    const bool wasPlaying = m_playbackActive;
    m_playbackActive = false;
    stopClock();
    m_usingFallbackClock = false;
    m_waitingForMediaClock = false;
    m_audioBuffer.close();
    m_audioBuffer.setBuffer(nullptr);
    m_audioData.clear();
    resetMediaPlayer();
    m_audioSource = source;
    m_hasAudioSource = !source.isEmpty();
    m_mediaPlayer->setSource(source);
    m_mediaPlayer->setPlaybackRate(m_playbackRate);
    m_mediaPlayer->setPitchCompensation(m_playbackRate < kFullPlaybackRate);
    m_mediaPlayer->setPosition(m_positionMilliseconds);
    if (wasPlaying) {
        emit playbackChanged(false);
    }
}

void PlaybackController::setAudioData(QByteArray audioData, const QUrl& sourceHint) {
    const bool wasPlaying = m_playbackActive;
    m_playbackActive = false;
    stopClock();
    m_usingFallbackClock = false;
    m_waitingForMediaClock = false;
    m_audioBuffer.close();
    m_audioData = std::move(audioData);
    resetMediaPlayer();
    m_audioSource = sourceHint;
    m_hasAudioSource = !m_audioData.isEmpty();
    if (m_hasAudioSource) {
        m_audioBuffer.setBuffer(&m_audioData);
        m_audioBuffer.open(QIODevice::ReadOnly);
        m_mediaPlayer->setSourceDevice(&m_audioBuffer, sourceHint);
    }
    m_mediaPlayer->setPlaybackRate(m_playbackRate);
    m_mediaPlayer->setPitchCompensation(m_playbackRate < kFullPlaybackRate);
    m_mediaPlayer->setPosition(m_positionMilliseconds);
    if (wasPlaying) {
        emit playbackChanged(false);
    }
}

void PlaybackController::setPosition(const qint64 positionMilliseconds) {
    const qint64 clampedPosition = std::clamp(positionMilliseconds, qint64(0), m_chartDurationMilliseconds);
    if (m_positionMilliseconds == clampedPosition) {
        return;
    }

    m_positionMilliseconds = clampedPosition;
    if (m_playbackActive && !m_usingFallbackClock) {
        m_mediaClockStartPositionMilliseconds = clampedPosition;
        m_waitingForMediaClock = true;
        stopClock();
    }
    if (m_hasAudioSource) {
        m_mediaPlayer->setPosition(clampedPosition);
    }
    if (m_playbackActive) {
        if (m_usingFallbackClock) {
            restartClock(clampedPosition);
        }
    }
    emit positionChanged(m_positionMilliseconds);
}

void PlaybackController::setVolume(const qreal volume) {
    m_audioOutput.setVolume(std::clamp(volume, qreal(0.0), qreal(1.0)));
}

void PlaybackController::togglePlayback() {
    if (isPlaying()) {
        publishPosition(m_waitingForMediaClock ? m_positionMilliseconds : clockPosition());
        stopClock();
        m_mediaPlayer->pause();
        m_playbackActive = false;
        m_waitingForMediaClock = false;
        emit playbackChanged(false);
        return;
    }

    if (m_positionMilliseconds >= m_chartDurationMilliseconds) {
        setPosition(0);
    }
    m_playbackActive = true;
    m_usingFallbackClock = !m_hasAudioSource;
    if (m_usingFallbackClock) {
        restartClock(m_positionMilliseconds);
    } else {
        m_mediaClockStartPositionMilliseconds = m_positionMilliseconds;
        m_waitingForMediaClock = true;
        m_mediaPlayer->play();
    }
    emit playbackChanged(true);
}

void PlaybackController::scrubBy(const qint64 offsetMilliseconds) {
    setPosition(m_positionMilliseconds + offsetMilliseconds);
}

void PlaybackController::increasePlaybackRate() {
    changePlaybackRate(-1);
}

void PlaybackController::decreasePlaybackRate() {
    changePlaybackRate(1);
}

void PlaybackController::updateClock() {
    if (!m_playbackActive || m_waitingForMediaClock) {
        stopClock();
        return;
    }

    const qint64 updatedPosition = clockPosition();
    if (updatedPosition >= m_chartDurationMilliseconds) {
        publishPosition(m_chartDurationMilliseconds);
        stopClock();
        if (!m_usingFallbackClock) {
            m_mediaPlayer->pause();
        }
        m_playbackActive = false;
        emit playbackChanged(false);
        return;
    }
    publishPosition(updatedPosition);
}

void PlaybackController::handleMediaPosition(const qint64 positionMilliseconds) {
    if (!m_playbackActive || m_usingFallbackClock) {
        return;
    }

    const qint64 clampedPosition = std::clamp(positionMilliseconds, qint64(0), m_chartDurationMilliseconds);
    if (m_waitingForMediaClock) {
        if (clampedPosition <= m_mediaClockStartPositionMilliseconds) {
            return;
        }

        m_waitingForMediaClock = false;
        restartClock(clampedPosition);
        publishPosition(clampedPosition);
        return;
    }
}

void PlaybackController::handleMediaPlaybackState(const QMediaPlayer::PlaybackState state) {
    if (state != QMediaPlayer::StoppedState || !m_playbackActive || m_usingFallbackClock) {
        return;
    }

    publishPosition(clockPosition());
    stopClock();
    m_playbackActive = false;
    m_waitingForMediaClock = false;
    emit playbackChanged(false);
}

void PlaybackController::handleMediaError(const QMediaPlayer::Error error, const QString& message) {
    Q_UNUSED(error)

    if (!message.isEmpty()) {
        emit audioError(message);
    }
    if (m_playbackActive && !m_usingFallbackClock) {
        publishPosition(m_waitingForMediaClock ? m_positionMilliseconds : clockPosition());
        m_usingFallbackClock = true;
        m_waitingForMediaClock = false;
        restartClock(m_positionMilliseconds);
        m_mediaPlayer->stop();
    }
}

qint64 PlaybackController::clockPosition() const {
    const auto elapsed = std::chrono::steady_clock::now() - m_clockAnchor;
    const auto elapsedMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();

    return m_clockAnchorPositionMilliseconds
        + static_cast<qint64>(std::llround(static_cast<qreal>(elapsedMilliseconds) * m_playbackRate));
}

void PlaybackController::changePlaybackRate(const int direction) {
    const auto current = std::find(kPlaybackRates.cbegin(), kPlaybackRates.cend(), m_playbackRate);
    const int currentIndex = current == kPlaybackRates.cend()
        ? 0
        : static_cast<int>(std::distance(kPlaybackRates.cbegin(), current));
    const int requestedIndex = std::clamp(currentIndex + direction, 0, static_cast<int>(kPlaybackRates.size()) - 1);

    setPlaybackRate(kPlaybackRates.at(static_cast<std::size_t>(requestedIndex)));
}

void PlaybackController::restartClock(const qint64 positionMilliseconds) {
    m_clockAnchorPositionMilliseconds = positionMilliseconds;
    m_clockAnchor = std::chrono::steady_clock::now();
    m_clockUpdateTimer.start();
}

void PlaybackController::setPlaybackRate(const qreal playbackRate) {
    const qreal clampedRate = std::clamp(playbackRate, kPlaybackRates.back(), kPlaybackRates.front());
    if (qFuzzyCompare(m_playbackRate, clampedRate)) {
        return;
    }

    const qint64 currentPosition = m_playbackActive && !m_waitingForMediaClock
        ? clockPosition()
        : m_positionMilliseconds;
    m_playbackRate = clampedRate;
    m_mediaPlayer->setPlaybackRate(m_playbackRate);
    m_mediaPlayer->setPitchCompensation(m_playbackRate < kFullPlaybackRate);
    if (m_playbackActive) {
        publishPosition(currentPosition);
        if (m_usingFallbackClock) {
            restartClock(m_positionMilliseconds);
        } else {
            m_mediaClockStartPositionMilliseconds = m_positionMilliseconds;
            m_waitingForMediaClock = true;
            stopClock();
        }
    }
    emit playbackRateChanged(m_playbackRate);
}

void PlaybackController::stopClock() {
    m_clockUpdateTimer.stop();
}

void PlaybackController::resetMediaPlayer() {
    // Make each source own a player. Destroying the old player prevents an
    // asynchronous backend duration from being delivered for a later source.
    m_mediaPlayer = std::make_unique<QMediaPlayer>();
    m_mediaPlayer->setAudioOutput(&m_audioOutput);
    connect(m_mediaPlayer.get(), &QMediaPlayer::positionChanged, this, &PlaybackController::handleMediaPosition);
    connect(m_mediaPlayer.get(), &QMediaPlayer::durationChanged, this, &PlaybackController::mediaDurationChanged);
    connect(m_mediaPlayer.get(), &QMediaPlayer::playbackStateChanged, this, &PlaybackController::handleMediaPlaybackState);
    connect(m_mediaPlayer.get(), &QMediaPlayer::errorOccurred, this, &PlaybackController::handleMediaError);
}

void PlaybackController::publishPosition(const qint64 positionMilliseconds) {
    const qint64 clampedPosition = std::clamp(positionMilliseconds, qint64(0), m_chartDurationMilliseconds);
    if (m_positionMilliseconds == clampedPosition) {
        return;
    }
    m_positionMilliseconds = clampedPosition;
    emit positionChanged(m_positionMilliseconds);
}
