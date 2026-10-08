#include "core/waveform.h"

#include <algorithm>

namespace infalsus {

void buildWaveform(QPromise<WaveformData>& promise, const QByteArray& samples, const QAudioFormat& format) {
    WaveformData waveform;
    const int sampleRate = format.sampleRate();
    const int channelCount = format.channelCount();

    if (promise.isCanceled()) {
        return;
    }
    if (sampleRate <= 0 || channelCount <= 0 || format.sampleFormat() != QAudioFormat::Float) {
        promise.addResult(std::move(waveform));
        return;
    }
    const float* audio = reinterpret_cast<const float*>(samples.constData());
    const qsizetype frameCount = samples.size() / format.bytesPerFrame();
    const int framesPerPeak = std::max(1, sampleRate / kWaveformPointsPerSecond);
    const qsizetype peakCount = (frameCount + framesPerPeak - 1) / framesPerPeak;

    waveform.millisecondsPerPeak = 1000.0 * framesPerPeak / sampleRate;
    waveform.peaks.reserve(peakCount);
    for (qsizetype firstFrame = 0; firstFrame < frameCount; firstFrame += framesPerPeak) {
        if (promise.isCanceled()) {
            return;
        }
        const qsizetype lastFrame = std::min(firstFrame + framesPerPeak, frameCount);
        const qsizetype lastSample = lastFrame * channelCount;
        float minimum = 1.0f;
        float maximum = -1.0f;

        for (qsizetype sample = firstFrame * channelCount; sample < lastSample; ++sample) {
            minimum = std::min(minimum, audio[sample]);
            maximum = std::max(maximum, audio[sample]);
        }
        waveform.peaks.append(QPointF(std::clamp(minimum, -1.0f, 1.0f), std::clamp(maximum, -1.0f, 1.0f)));
    }
    promise.addResult(std::move(waveform));
}

} // namespace infalsus
