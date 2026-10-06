#pragma once

#include <QtCore/QVector>

#include <memory>

namespace infalsus {

struct TimingEstimate {
    double beatsPerMinute = 120.0;
    qint64 offsetMilliseconds = 0;
    int supportingIntervals = 0;
    int totalIntervals = 0;

    [[nodiscard]] bool isValid() const { return supportingIntervals > 0; }
    [[nodiscard]] double confidence() const {
        return totalIntervals > 0 ? static_cast<double>(supportingIntervals) / totalIntervals : 0.0;
    }
};

class InitialTimingAnalyzer {
public:
    explicit InitialTimingAnalyzer(int sampleRate);
    ~InitialTimingAnalyzer();

    InitialTimingAnalyzer(InitialTimingAnalyzer&&) noexcept;
    InitialTimingAnalyzer& operator=(InitialTimingAnalyzer&&) noexcept;
    InitialTimingAnalyzer(const InitialTimingAnalyzer&) = delete;
    InitialTimingAnalyzer& operator=(const InitialTimingAnalyzer&) = delete;

    void appendSamples(const float* samples, qsizetype sampleCount);
    [[nodiscard]] qint64 sampleCount() const;
    [[nodiscard]] TimingEstimate estimate() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Estimates a constant tempo and beat-grid phase from mono PCM samples. The
// tempo vote follows TimingAnalyz's 100-150 Hz peak-interval technique; the
// phase is derived from the same retained beat peaks.
[[nodiscard]] TimingEstimate estimateInitialTiming(const QVector<float>& monoSamples, int sampleRate);

} // namespace infalsus
