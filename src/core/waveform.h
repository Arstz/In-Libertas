#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QPointF>
#include <QtCore/QPromise>
#include <QtCore/QVector>
#include <QtMultimedia/QAudioFormat>

namespace infalsus {

inline constexpr int kWaveformPointsPerSecond = 720;

struct WaveformData {
    QVector<QPointF> peaks;
    double millisecondsPerPeak = 0.0;
};

void buildWaveform(QPromise<WaveformData>& promise, const QByteArray& samples, const QAudioFormat& format);

} // namespace infalsus
