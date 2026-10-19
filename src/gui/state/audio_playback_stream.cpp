#include "gui/state/audio_playback_stream.h"

#include <signalsmith-stretch/signalsmith-stretch.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

constexpr int kProcessingIntervalMicroseconds = 10000;
constexpr qreal kNormalPlaybackRate = 1.0;

struct AudioInput {
    struct Channel {
        const float* samples;
        qint64 firstFrame;
        qint64 frameCount;
        int channelCount;
        int channelIndex;

        [[nodiscard]] float operator[](const int index) const {
            const qint64 frame = firstFrame + index;

            return frame >= 0 && frame < frameCount ? samples[frame * channelCount + channelIndex] : 0.0f;
        }
    };

    const float* samples;
    qint64 firstFrame;
    qint64 frameCount;
    int channelCount;

    [[nodiscard]] Channel operator[](const int index) const {
        return {samples, firstFrame, frameCount, channelCount, index};
    }
};

struct AudioOutput {
    struct Channel {
        float* samples;
        int channelCount;
        int channelIndex;

        [[nodiscard]] float& operator[](const int index) const {
            return samples[index * channelCount + channelIndex];
        }
    };

    float* samples;
    int channelCount;

    [[nodiscard]] Channel operator[](const int index) const {
        return {samples, channelCount, index};
    }
};

} // namespace

struct AudioPlaybackStream::ProcessingState {
    struct Sound {
        QByteArray samples;
        qint64 firstFrame = 0;
        qint64 lastFrame = 0;
        HitsoundKind kind = HitsoundKind::NoteHit;
        bool loop = false;
    };

    ProcessingState(std::shared_ptr<AudioPlaybackData> samples, const QAudioFormat& audioFormat,
        const qint64 positionMilliseconds, const qint64 durationMilliseconds, const qreal rate)
        : source(std::move(samples)), format(audioFormat), playbackRate(rate) {
        const qint64 firstFrame = format.framesForDuration(positionMilliseconds * 1000);
        const qint64 lastFrame = format.framesForDuration(durationMilliseconds * 1000);

        startMilliseconds = positionMilliseconds;
        sourceFrame = firstFrame;
        totalOutputFrames = static_cast<qint64>(std::ceil(std::max<qint64>(0, lastFrame - firstFrame) / playbackRate));
        processingFrames = std::max(1, format.framesForDuration(kProcessingIntervalMicroseconds));
        if (playbackRate < kNormalPlaybackRate) {
            stretcher.presetDefault(format.channelCount(), format.sampleRate());
            sourceFrame += stretcher.inputLatency();
            primed = false;
        }
    }

    [[nodiscard]] AudioInput inputAt(const qint64 frame) const {
        return {reinterpret_cast<const float*>(source->samples.constData()), frame,
            source->samples.size() / format.bytesPerFrame(), format.channelCount()};
    }

    [[nodiscard]] qint64 availableOutputFrames() const {
        const qint64 remainingFrames = totalOutputFrames - generatedOutputFrames;
        if (source->complete) {
            return remainingFrames;
        }
        qint64 inputEnd = sourceFrame;
        qreal fraction = fractionalInputFrames;
        if (!primed) {
            const qreal primeInput = stretcher.outputLatency() * playbackRate;
            const auto primeFrames = static_cast<qint64>(std::floor(primeInput));
            inputEnd += primeFrames;
            fraction = primeInput - primeFrames;
        }
        const qint64 availableInput = source->samples.size() / format.bytesPerFrame() - inputEnd;
        if (availableInput < 0) {
            return 0;
        }
        qint64 outputFrames = std::clamp<qint64>(
            static_cast<qint64>(std::ceil((availableInput + 1 - fraction) / playbackRate)) - 1,
            0, remainingFrames);
        if (outputFrames < remainingFrames) {
            outputFrames -= outputFrames % processingFrames;
        }
        return outputFrames;
    }

    void process(const int outputFrames) {
        const qreal expectedInputFrames = outputFrames * playbackRate + fractionalInputFrames;
        const int inputFrames = static_cast<int>(std::floor(expectedInputFrames));

        fractionalInputFrames = expectedInputFrames - inputFrames;
        pending.resize(outputFrames * format.bytesPerFrame());
        stretcher.process(inputAt(sourceFrame), inputFrames,
            AudioOutput{reinterpret_cast<float*>(pending.data()), format.channelCount()}, outputFrames);
        sourceFrame += inputFrames;
    }

    bool generate() {
        const int outputFrames = static_cast<int>(std::min<qint64>(processingFrames, availableOutputFrames()));
        if (outputFrames == 0) {
            return false;
        }
        if (!primed) {
            const int historyFrames = stretcher.blockSamples() + stretcher.intervalSamples();
            stretcher.seek(inputAt(sourceFrame - historyFrames), historyFrames, playbackRate);
            process(stretcher.outputLatency());
            pending.clear();
            primed = true;
        }

        pendingOffset = 0;
        if (playbackRate < kNormalPlaybackRate) {
            process(outputFrames);
        } else {
            const qint64 sourceFrames = source->samples.size() / format.bytesPerFrame();
            const qint64 availableFrames = std::clamp(sourceFrames - sourceFrame, qint64(0), qint64(outputFrames));
            const qsizetype availableBytes = availableFrames * format.bytesPerFrame();

            pending.resize(outputFrames * format.bytesPerFrame());
            if (availableBytes > 0) {
                std::memcpy(pending.data(), source->samples.constData() + sourceFrame * format.bytesPerFrame(), availableBytes);
            }
            std::memset(pending.data() + availableBytes, 0, pending.size() - availableBytes);
            sourceFrame += outputFrames;
        }
        mixSounds(outputFrames);
        generatedOutputFrames += outputFrames;
        return true;
    }

    void mixSounds(const int outputFrames) {
        if (!mix) {
            return;
        }
        std::array<QByteArray, kHitsoundDescriptions.size()> soundSamples;
        {
            const std::lock_guard lock(mix->mutex);
            soundSamples = mix->sounds;
        }
        {
            const std::lock_guard lock(soundsMutex);
            if (soundsChanged) {
                sounds.swap(pendingSounds);
                pendingSounds.clear();
                nextSound = 0;
                activeSounds.removeIf([](const Sound& sound) { return sound.loop; });
                soundsChanged = false;
            }
        }
        QVector<QByteArray> incomingPreviews;
        std::array<bool, kHitsoundDescriptions.size()> requestedLoops;
        std::array<quint64, kHitsoundDescriptions.size()> requestedGenerations;
        {
            const std::lock_guard lock(previewMutex);
            incomingPreviews.swap(previews);
            requestedLoops = liveLoops;
            requestedGenerations = liveLoopGenerations;
        }
        const qint64 blockEnd = generatedOutputFrames + outputFrames;
        const int channels = format.channelCount();
        auto* output = reinterpret_cast<float*>(pending.data());
        const qreal songVolume = mix->songVolume.load();
        const qreal effectsVolume = mix->effectsVolume.load();

        for (int index = 0; index < outputFrames * channels; ++index) {
            output[index] *= songVolume;
        }
        while (nextSound < sounds.size() && sounds[nextSound].firstFrame < blockEnd) {
            const Sound& sound = sounds[nextSound++];
            if (sound.loop ? sound.lastFrame > generatedOutputFrames : sound.firstFrame >= generatedOutputFrames) {
                activeSounds.append(sound);
            }
        }
        for (QByteArray& samples : incomingPreviews) {
            activeSounds.append({std::move(samples), generatedOutputFrames, 0});
        }
        for (Sound& sound : activeSounds) {
            const QByteArray& samples = sound.samples.isEmpty() ? soundSamples[static_cast<int>(sound.kind)] : sound.samples;
            const qint64 sampleFrames = samples.size() / format.bytesPerFrame();
            if (sampleFrames == 0) {
                continue;
            }
            const qint64 lastFrame = sound.loop ? sound.lastFrame : sound.firstFrame + sampleFrames;
            const qint64 first = std::max(generatedOutputFrames, sound.firstFrame);
            const qint64 last = std::min(blockEnd, lastFrame);
            const auto* input = reinterpret_cast<const float*>(samples.constData());
            for (qint64 frame = first; frame < last; ++frame) {
                const qint64 inputFrame = (frame - sound.firstFrame) % sampleFrames;
                const qint64 outputFrame = frame - generatedOutputFrames;
                for (int channel = 0; channel < channels; ++channel) {
                    output[outputFrame * channels + channel] += input[inputFrame * channels + channel] * effectsVolume;
                }
            }
        }
        for (int kind = 0; kind < static_cast<int>(liveLoops.size()); ++kind) {
            if (mixedLoopGenerations[kind] != requestedGenerations[kind]) {
                liveLoopFrames[kind] = 0;
                mixedLoopGenerations[kind] = requestedGenerations[kind];
            }
            if (!requestedLoops[kind]) {
                continue;
            }
            const QByteArray& samples = soundSamples[kind];
            const qint64 frames = samples.size() / format.bytesPerFrame();
            if (frames == 0) {
                continue;
            }
            const auto* input = reinterpret_cast<const float*>(samples.constData());
            for (int frame = 0; frame < outputFrames; ++frame) {
                const qint64 inputFrame = (liveLoopFrames[kind] + frame) % frames;
                for (int channel = 0; channel < channels; ++channel) {
                    output[frame * channels + channel] += input[inputFrame * channels + channel] * effectsVolume;
                }
            }
            liveLoopFrames[kind] += outputFrames;
        }
        activeSounds.removeIf([&](const Sound& sound) {
            const QByteArray& samples = sound.samples.isEmpty() ? soundSamples[static_cast<int>(sound.kind)] : sound.samples;
            return (sound.loop ? sound.lastFrame : sound.firstFrame + samples.size() / format.bytesPerFrame()) <= blockEnd;
        });
        for (int index = 0; index < outputFrames * channels; ++index) {
            output[index] = std::clamp(output[index], -1.0f, 1.0f);
        }
    }

    std::shared_ptr<AudioPlaybackData> source;
    std::shared_ptr<AudioMixData> mix;
    QVector<Sound> sounds;
    QVector<Sound> pendingSounds;
    QVector<Sound> activeSounds;
    QVector<QByteArray> previews;
    std::array<bool, kHitsoundDescriptions.size()> liveLoops{};
    std::array<quint64, kHitsoundDescriptions.size()> liveLoopGenerations{};
    std::array<quint64, kHitsoundDescriptions.size()> mixedLoopGenerations{};
    std::array<qint64, kHitsoundDescriptions.size()> liveLoopFrames{};
    std::mutex previewMutex;
    std::mutex soundsMutex;
    QByteArray pending;
    QAudioFormat format;
    signalsmith::stretch::SignalsmithStretch<float> stretcher{0};
    qint64 sourceFrame = 0;
    qint64 totalOutputFrames = 0;
    qint64 generatedOutputFrames = 0;
    qint64 bytesRead = 0;
    qint64 startMilliseconds = 0;
    qsizetype nextSound = 0;
    qsizetype pendingOffset = 0;
    int processingFrames = 0;
    qreal playbackRate = kNormalPlaybackRate;
    qreal fractionalInputFrames = 0.0;
    bool primed = true;
    bool soundsChanged = false;
};

AudioPlaybackStream::AudioPlaybackStream(QByteArray samples, const QAudioFormat& format,
    const qint64 positionMilliseconds, const qint64 durationMilliseconds, const qreal playbackRate)
    : AudioPlaybackStream(std::make_shared<AudioPlaybackData>(AudioPlaybackData{std::move(samples), true}),
          format, positionMilliseconds, durationMilliseconds, playbackRate) {
}

AudioPlaybackStream::AudioPlaybackStream(std::shared_ptr<AudioPlaybackData> source, const QAudioFormat& format,
    const qint64 positionMilliseconds, const qint64 durationMilliseconds, const qreal playbackRate)
    : m_processing(std::make_unique<ProcessingState>(std::move(source), format,
          positionMilliseconds, durationMilliseconds, playbackRate)) {
    open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

AudioPlaybackStream::~AudioPlaybackStream() = default;

void AudioPlaybackStream::sourceUpdated() {
    if (bytesAvailable() > 0) {
        emit readyRead();
    }
}

void AudioPlaybackStream::setMix(std::shared_ptr<AudioMixData> mix, const QVector<HitsoundEvent>& events) {
    m_processing->mix = std::move(mix);
    setHitsoundEvents(events);
}

void AudioPlaybackStream::setHitsoundEvents(const QVector<HitsoundEvent>& events) {
    auto& state = *m_processing;
    QVector<ProcessingState::Sound> sounds;
    sounds.reserve(events.size());
    for (const HitsoundEvent& event : events) {
        if ((!event.loop && event.startMilliseconds < state.startMilliseconds)
            || (event.loop && event.endMilliseconds <= state.startMilliseconds)) {
            continue;
        }
        const auto outputFrame = [&state](const qint64 time) {
            return static_cast<qint64>(std::llround(
                (time - state.startMilliseconds) * state.format.sampleRate() / (1000.0 * state.playbackRate)));
        };
        sounds.append({{}, outputFrame(event.startMilliseconds), outputFrame(event.endMilliseconds),
            event.kind, event.loop});
    }
    std::stable_sort(sounds.begin(), sounds.end(), [](const auto& first, const auto& second) {
        return first.firstFrame < second.firstFrame;
    });
    const std::lock_guard lock(state.soundsMutex);
    state.pendingSounds = std::move(sounds);
    state.soundsChanged = true;
}

void AudioPlaybackStream::preview(QByteArray samples) {
    const std::lock_guard lock(m_processing->previewMutex);
    m_processing->previews.append(std::move(samples));
}

void AudioPlaybackStream::setLiveLoop(const HitsoundKind kind, const bool active) {
    const std::lock_guard lock(m_processing->previewMutex);
    const int index = static_cast<int>(kind);
    if (index < 0 || index >= static_cast<int>(m_processing->liveLoops.size())) {
        return;
    }
    if (m_processing->liveLoops[index] != active) {
        m_processing->liveLoops[index] = active;
        ++m_processing->liveLoopGenerations[index];
    }
}

bool AudioPlaybackStream::isSequential() const {
    return true;
}

bool AudioPlaybackStream::atEnd() const {
    return m_processing->bytesRead >= m_processing->totalOutputFrames * m_processing->format.bytesPerFrame();
}

qint64 AudioPlaybackStream::bytesAvailable() const {
    return m_processing->pending.size() - m_processing->pendingOffset
        + m_processing->availableOutputFrames() * m_processing->format.bytesPerFrame()
        + QIODevice::bytesAvailable();
}

qint64 AudioPlaybackStream::readData(char* data, const qint64 maximumSize) {
    qint64 copiedBytes = 0;

    while (copiedBytes < maximumSize && !atEnd()) {
        if (m_processing->pendingOffset == m_processing->pending.size()) {
            if (!m_processing->generate()) {
                break;
            }
        }
        const qint64 availableBytes = m_processing->pending.size() - m_processing->pendingOffset;
        const qint64 copySize = std::min(maximumSize - copiedBytes, availableBytes);

        std::memcpy(data + copiedBytes, m_processing->pending.constData() + m_processing->pendingOffset, copySize);
        m_processing->pendingOffset += copySize;
        m_processing->bytesRead += copySize;
        copiedBytes += copySize;
    }

    return copiedBytes;
}

qint64 AudioPlaybackStream::writeData(const char*, qint64) {
    return -1;
}
