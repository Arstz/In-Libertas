#include "core/timing_analyzer.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>

namespace infalsus {

namespace {

constexpr double kMinimumTempo = 90.0;
constexpr double kMaximumTempo = 180.0;
constexpr double kLowCutoffHertz = 100.0;
constexpr double kHighCutoffHertz = 150.0;
constexpr double kFilterQ = 1.0;

struct Peak {
    int position = 0;
    float volume = 0.0F;
};

class BiquadFilter {
public:
    enum class Type {
        LowPass,
        HighPass,
    };

    BiquadFilter(const Type type, const int sampleRate, const double cutoffHertz, const double quality) {
        const double omega = 2.0 * std::numbers::pi * cutoffHertz / sampleRate;
        const double sine = std::sin(omega);
        const double cosine = std::cos(omega);
        const double alpha = sine / (2.0 * quality);
        const double a0 = 1.0 + alpha;
        if (type == Type::LowPass) {
            m_b0 = (1.0 - cosine) * 0.5 / a0;
            m_b1 = (1.0 - cosine) / a0;
            m_b2 = m_b0;
        } else {
            m_b0 = (1.0 + cosine) * 0.5 / a0;
            m_b1 = -(1.0 + cosine) / a0;
            m_b2 = m_b0;
        }
        m_a1 = -2.0 * cosine / a0;
        m_a2 = (1.0 - alpha) / a0;
    }

    [[nodiscard]] float transform(const float sample) {
        const double value = m_b0 * sample + m_b1 * m_x1 + m_b2 * m_x2 - m_a1 * m_y1 - m_a2 * m_y2;
        m_x2 = m_x1;
        m_x1 = sample;
        m_y2 = m_y1;
        m_y1 = value;
        return static_cast<float>(value);
    }

private:
    double m_b0 = 0.0;
    double m_b1 = 0.0;
    double m_b2 = 0.0;
    double m_a1 = 0.0;
    double m_a2 = 0.0;
    double m_x1 = 0.0;
    double m_x2 = 0.0;
    double m_y1 = 0.0;
    double m_y2 = 0.0;
};

[[nodiscard]] TimingEstimate estimateFromPeaks(QVector<Peak> peaks, const int sampleRate) {
    if (sampleRate <= 0 || peaks.size() < 20) {
        return {};
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& first, const Peak& second) {
        return first.volume > second.volume;
    });
    peaks.resize(peaks.size() / 2);
    if (peaks.size() < 2) {
        return {};
    }
    std::sort(peaks.begin(), peaks.end(), [](const Peak& first, const Peak& second) {
        return first.position < second.position;
    });

    std::map<int, int> tempoCounts;
    int totalIntervals = 0;
    for (int index = 0; index < peaks.size(); ++index) {
        for (int next = 1; next < 10 && index + next < peaks.size(); ++next) {
            const int interval = peaks.at(index + next).position - peaks.at(index).position;
            if (interval <= 0) {
                continue;
            }
            double tempo = 60.0 * sampleRate / interval;
            while (tempo < kMinimumTempo) {
                tempo *= 2.0;
            }
            while (tempo > kMaximumTempo) {
                tempo *= 0.5;
            }
            ++tempoCounts[static_cast<int>(std::lround(tempo))];
            ++totalIntervals;
        }
    }
    if (tempoCounts.empty()) {
        return {};
    }

    const auto winningTempo = std::max_element(tempoCounts.cbegin(), tempoCounts.cend(),
        [](const auto& first, const auto& second) {
            return first.second != second.second ? first.second < second.second : first.first > second.first;
        });
    // TimingAnalyz reports an additional double-time candidate below 110 BPM.
    // In Falsus charts commonly use that upper interpretation (for example,
    // 185 BPM is folded into the 90-180 BPM voting range as approximately 93).
    const double resolvedTempo = winningTempo->first < 110
        ? static_cast<double>(winningTempo->first * 2)
        : static_cast<double>(winningTempo->first);
    const double periodSamples = 60.0 * sampleRate / resolvedTempo;

    // A beat grid is periodic, so calculate its phase as a weighted circular
    // mean rather than treating peaks near either end of the period as far
    // apart. Stronger retained peaks contribute more to the phase.
    double cosine = 0.0;
    double sine = 0.0;
    for (const Peak& peak : peaks) {
        const double phase = std::fmod(static_cast<double>(peak.position), periodSamples);
        const double angle = 2.0 * std::numbers::pi * phase / periodSamples;
        const double weight = std::max(0.0, static_cast<double>(peak.volume));
        cosine += weight * std::cos(angle);
        sine += weight * std::sin(angle);
    }
    double phase = std::atan2(sine, cosine) * periodSamples / (2.0 * std::numbers::pi);
    if (phase < 0.0) {
        phase += periodSamples;
    }

    return {
        .beatsPerMinute = resolvedTempo,
        .offsetMilliseconds = static_cast<qint64>(std::llround(phase * 1000.0 / sampleRate)),
        .supportingIntervals = winningTempo->second,
        .totalIntervals = totalIntervals,
    };
}

} // namespace

class InitialTimingAnalyzer::Impl {
public:
    explicit Impl(const int sampleRate)
        : sampleRate(sampleRate),
          partSize(std::max(1, sampleRate / 2)),
          lowPass(BiquadFilter::Type::LowPass, sampleRate, kHighCutoffHertz, kFilterQ),
          highPass(BiquadFilter::Type::HighPass, sampleRate, kLowCutoffHertz, kFilterQ),
          currentPeak{.position = 0, .volume = std::numeric_limits<float>::lowest()} {}

    void append(const float* samples, const qsizetype count) {
        if (samples == nullptr || count <= 0) {
            return;
        }
        for (qsizetype index = 0; index < count; ++index) {
            const float value = highPass.transform(lowPass.transform(samples[index]));
            if (value > currentPeak.volume) {
                currentPeak = {.position = static_cast<int>(samplesProcessed), .volume = value};
            }
            ++samplesProcessed;
            if (++samplesInPart == partSize) {
                peaks.append(currentPeak);
                samplesInPart = 0;
                currentPeak = {
                    .position = static_cast<int>(samplesProcessed),
                    .volume = std::numeric_limits<float>::lowest(),
                };
            }
        }
    }

    int sampleRate = 0;
    int partSize = 1;
    int samplesInPart = 0;
    qint64 samplesProcessed = 0;
    BiquadFilter lowPass;
    BiquadFilter highPass;
    Peak currentPeak;
    QVector<Peak> peaks;
};

InitialTimingAnalyzer::InitialTimingAnalyzer(const int sampleRate)
    : m_impl(sampleRate > 0 ? std::make_unique<Impl>(sampleRate) : nullptr) {}

InitialTimingAnalyzer::~InitialTimingAnalyzer() = default;
InitialTimingAnalyzer::InitialTimingAnalyzer(InitialTimingAnalyzer&&) noexcept = default;
InitialTimingAnalyzer& InitialTimingAnalyzer::operator=(InitialTimingAnalyzer&&) noexcept = default;

void InitialTimingAnalyzer::appendSamples(const float* samples, const qsizetype sampleCount) {
    if (m_impl != nullptr) {
        m_impl->append(samples, sampleCount);
    }
}

qint64 InitialTimingAnalyzer::sampleCount() const {
    return m_impl != nullptr ? m_impl->samplesProcessed : 0;
}

TimingEstimate InitialTimingAnalyzer::estimate() const {
    return m_impl != nullptr ? estimateFromPeaks(m_impl->peaks, m_impl->sampleRate) : TimingEstimate{};
}

TimingEstimate estimateInitialTiming(const QVector<float>& monoSamples, const int sampleRate) {
    InitialTimingAnalyzer analyzer(sampleRate);
    analyzer.appendSamples(monoSamples.constData(), monoSamples.size());
    return analyzer.estimate();
}

} // namespace infalsus
