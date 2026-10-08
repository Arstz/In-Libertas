#include "gui/state/playback_controller.h"

#include "gui/state/audio_playback_stream.h"

#include <QtMultimedia/QMediaDevices>

#include <algorithm>
#include <chrono>
#include <cmath>

namespace {

constexpr int kClockUpdateIntervalMilliseconds = 4;
constexpr int kOutputBufferMicroseconds = 30000;
constexpr int kDefaultSampleRate = 48000;
constexpr int kDefaultChannelCount = 2;
using infalsus::gui::kFullPlaybackRate;
using infalsus::gui::kPlaybackRates;

} // namespace

PlaybackController::PlaybackController(QObject* parent)
    : QObject(parent) {
    m_clockUpdateTimer.setInterval(kClockUpdateIntervalMilliseconds);
    m_clockUpdateTimer.setTimerType(Qt::PreciseTimer);

    connect(&m_clockUpdateTimer, &QTimer::timeout, this, &PlaybackController::updateClock);
}

PlaybackController::~PlaybackController() {
    m_audioDecoder.reset();
    resetAudioPlayback();
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
    return m_volume;
}

void PlaybackController::setChartDuration(const qint64 durationMilliseconds) {
    const qint64 clampedDuration = std::max<qint64>(durationMilliseconds, 0);
    const qint64 currentPosition = m_playbackActive ? clockPosition() : m_positionMilliseconds;

    if (m_chartDurationMilliseconds == clampedDuration) {
        return;
    }
    m_chartDurationMilliseconds = clampedDuration;
    resetAudioPlayback();
    publishPosition(currentPosition);
    if (m_playbackActive) {
        if (m_usingFallbackClock) {
            restartClock(m_positionMilliseconds);
        } else {
            startAudioPlayback();
        }
    }
}

void PlaybackController::setAudioSource(const QUrl& source) {
    resetAudioSource();
    m_hasAudioSource = !source.isEmpty();
    if (!m_hasAudioSource) {
        return;
    }

    createAudioDecoder();
    m_audioDecoder->setSource(source);
    m_audioDecoder->start();
}

void PlaybackController::setAudioData(QByteArray audioData, const QUrl& sourceHint) {
    Q_UNUSED(sourceHint)

    resetAudioSource();
    m_audioData = std::move(audioData);
    m_hasAudioSource = !m_audioData.isEmpty();
    if (!m_hasAudioSource) {
        return;
    }

    m_audioBuffer.setBuffer(&m_audioData);
    m_audioBuffer.open(QIODevice::ReadOnly);
    createAudioDecoder();
    m_audioDecoder->setSourceDevice(&m_audioBuffer);
    m_audioDecoder->start();
}

void PlaybackController::setPosition(const qint64 positionMilliseconds) {
    const qint64 clampedPosition = std::clamp(positionMilliseconds, qint64(0), m_chartDurationMilliseconds);

    if (m_positionMilliseconds == clampedPosition) {
        return;
    }
    m_positionMilliseconds = clampedPosition;
    resetAudioPlayback();
    if (m_playbackActive) {
        if (m_usingFallbackClock) {
            restartClock(clampedPosition);
        } else {
            startAudioPlayback();
        }
    }
    emit positionChanged(m_positionMilliseconds);
}

void PlaybackController::setPlaybackRate(const qreal playbackRate) {
    const qreal clampedRate = std::clamp(playbackRate, kPlaybackRates.back(), kPlaybackRates.front());
    const qint64 currentPosition = m_playbackActive ? clockPosition() : m_positionMilliseconds;

    if (!std::isfinite(clampedRate) || qFuzzyCompare(m_playbackRate, clampedRate)) {
        return;
    }
    m_playbackRate = clampedRate;
    resetAudioPlayback();
    publishPosition(currentPosition);
    if (m_playbackActive) {
        if (m_usingFallbackClock) {
            restartClock(m_positionMilliseconds);
        } else {
            startAudioPlayback();
        }
    }
    emit playbackRateChanged(m_playbackRate);
}

void PlaybackController::setVolume(const qreal volume) {
    m_volume = std::clamp(volume, qreal(0.0), qreal(1.0));
    if (m_audioSink) {
        m_audioSink->setVolume(m_volume);
    }
}

void PlaybackController::togglePlayback() {
    if (isPlaying()) {
        if (m_audioSink) {
            m_audioSink->suspend();
        }
        const qint64 currentPosition = clockPosition();

        m_playbackActive = false;
        stopClock();
        publishPosition(currentPosition);
        emit playbackChanged(false);
        return;
    }

    if (m_positionMilliseconds >= m_chartDurationMilliseconds) {
        setPosition(0);
    }
    m_playbackActive = true;
    m_usingFallbackClock = !m_hasAudioSource || m_audioFailed;
    if (m_usingFallbackClock) {
        restartClock(m_positionMilliseconds);
    } else if (m_audioSink) {
        m_audioSink->resume();
        m_clockUpdateTimer.start();
    } else {
        startAudioPlayback();
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
    if (!m_playbackActive) {
        stopClock();
        return;
    }
    if (m_audioSink && m_audioSink->error() != QAudio::NoError && m_audioSink->error() != QAudio::UnderrunError) {
        handleAudioError(QStringLiteral("Audio output failed (%1).").arg(static_cast<int>(m_audioSink->error())));
        return;
    }

    const qint64 updatedPosition = clockPosition();
    const bool audioFinished = m_audioSink && m_audioStream->atEnd() && m_audioSink->state() == QAudio::IdleState;

    if ((!m_audioSink && updatedPosition >= m_chartDurationMilliseconds) || audioFinished) {
        m_playbackActive = false;
        resetAudioPlayback();
        stopClock();
        publishPosition(m_chartDurationMilliseconds);
        emit playbackChanged(false);
        return;
    }
    publishPosition(updatedPosition);
}

void PlaybackController::appendDecodedAudio() {
    if (m_audioFailed) {
        return;
    }
    while (m_audioDecoder->bufferAvailable()) {
        const QAudioBuffer buffer = m_audioDecoder->read();

        if (!buffer.isValid()) {
            continue;
        }
        if (buffer.format() != m_audioFormat) {
            handleAudioError(QStringLiteral("The audio decoder returned an unsupported playback format."));
            return;
        }
        m_decodedAudio->samples.append(buffer.constData<char>(), buffer.byteCount());
    }
    if (m_audioStream) {
        m_audioStream->sourceUpdated();
    }
    if (m_playbackActive && !m_usingFallbackClock) {
        startAudioPlayback();
    }
}

void PlaybackController::finishDecoding() {
    appendDecodedAudio();
    if (m_audioFailed) {
        return;
    }
    if (m_decodedAudio->samples.isEmpty()) {
        handleAudioError(QStringLiteral("The song contains no decodable audio."));
        return;
    }

    m_decodedAudio->complete = true;
    if (m_audioStream) {
        m_audioStream->sourceUpdated();
    }
    emit mediaDurationChanged(m_audioFormat.durationForBytes(m_decodedAudio->samples.size()) / 1000);
    emit audioDecoded(m_decodedAudio->samples, m_audioFormat);
    if (m_playbackActive && !m_usingFallbackClock) {
        startAudioPlayback();
    }
}

qint64 PlaybackController::clockPosition() const {
    if (!m_usingFallbackClock) {
        if (!m_audioSink) {
            return m_positionMilliseconds;
        }

        return m_audioStartPositionMilliseconds
            + static_cast<qint64>(std::llround(m_audioSink->processedUSecs() * m_playbackRate / 1000.0));
    }
    const auto elapsed = std::chrono::steady_clock::now() - m_clockAnchor;
    const qreal elapsedMilliseconds = std::chrono::duration<qreal, std::milli>(elapsed).count();

    return m_clockAnchorPositionMilliseconds
        + static_cast<qint64>(std::llround(elapsedMilliseconds * m_playbackRate));
}

void PlaybackController::resetAudioSource() {
    const bool wasPlaying = m_playbackActive;

    m_playbackActive = false;
    stopClock();
    m_audioDecoder.reset();
    resetAudioPlayback();
    m_audioBuffer.close();
    m_audioBuffer.setBuffer(nullptr);
    m_audioData.clear();
    m_decodedAudio.reset();
    m_audioFormat = {};
    m_hasAudioSource = false;
    m_usingFallbackClock = false;
    m_audioFailed = false;
    emit audioSourceChanged();
    if (wasPlaying) {
        emit playbackChanged(false);
    }
}

void PlaybackController::resetAudioPlayback() {
    if (m_audioSink) {
        m_audioSink->reset();
        m_audioSink.reset();
    }
    m_audioStream.reset();
}

void PlaybackController::createAudioDecoder() {
    m_audioFormat = QMediaDevices::defaultAudioOutput().preferredFormat();
    if (!m_audioFormat.isValid()) {
        m_audioFormat.setSampleRate(kDefaultSampleRate);
        m_audioFormat.setChannelCount(kDefaultChannelCount);
    }
    m_audioFormat.setSampleFormat(QAudioFormat::Float);
    m_decodedAudio = std::make_shared<AudioPlaybackData>();
    m_decodedAudio->complete = false;
    m_audioDecoder = std::make_unique<QAudioDecoder>();
    m_audioDecoder->setAudioFormat(m_audioFormat);
    connect(m_audioDecoder.get(), &QAudioDecoder::bufferReady, this, &PlaybackController::appendDecodedAudio);
    connect(m_audioDecoder.get(), &QAudioDecoder::finished, this, &PlaybackController::finishDecoding);
    connect(m_audioDecoder.get(), &QAudioDecoder::durationChanged, this, &PlaybackController::mediaDurationChanged);
    connect(m_audioDecoder.get(), qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
        [this](QAudioDecoder::Error) { handleAudioError(m_audioDecoder->errorString()); });
}

void PlaybackController::startAudioPlayback() {
    if (m_audioSink || !m_decodedAudio || m_decodedAudio->samples.isEmpty() || m_audioFailed
        || m_chartDurationMilliseconds <= m_positionMilliseconds) {
        return;
    }

    m_audioStartPositionMilliseconds = m_positionMilliseconds;
    if (!m_audioStream) {
        m_audioStream = std::make_unique<AudioPlaybackStream>(m_decodedAudio, m_audioFormat,
            m_positionMilliseconds, m_chartDurationMilliseconds, m_playbackRate);
    }
    if (m_audioStream->bytesAvailable() == 0) {
        return;
    }
    m_audioSink = std::make_unique<QAudioSink>(m_audioFormat);
    m_audioSink->setBufferSize(m_audioFormat.bytesForDuration(kOutputBufferMicroseconds));
    m_audioSink->setVolume(m_volume);
    m_audioSink->start(m_audioStream.get());
    if (m_audioSink->error() != QAudio::NoError) {
        handleAudioError(QStringLiteral("Could not start audio output (%1).").arg(static_cast<int>(m_audioSink->error())));
        return;
    }
    m_clockUpdateTimer.start();
}

void PlaybackController::handleAudioError(const QString& message) {
    const qint64 currentPosition = m_playbackActive ? clockPosition() : m_positionMilliseconds;

    if (m_audioFailed) {
        return;
    }
    m_audioFailed = true;
    resetAudioPlayback();
    if (m_playbackActive) {
        m_usingFallbackClock = true;
        publishPosition(currentPosition);
        restartClock(m_positionMilliseconds);
    }
    if (!message.isEmpty()) {
        emit audioError(message);
    }
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

void PlaybackController::stopClock() {
    m_clockUpdateTimer.stop();
}

void PlaybackController::publishPosition(const qint64 positionMilliseconds) {
    const qint64 clampedPosition = std::clamp(positionMilliseconds, qint64(0), m_chartDurationMilliseconds);

    if (m_positionMilliseconds == clampedPosition) {
        return;
    }
    m_positionMilliseconds = clampedPosition;
    emit positionChanged(m_positionMilliseconds);
}
