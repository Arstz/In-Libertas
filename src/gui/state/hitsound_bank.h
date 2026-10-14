#pragma once

#include "core/chart_types.h"

#include <QtCore/QObject>
#include <QtMultimedia/QAudioFormat>

#include <array>
#include <atomic>
#include <memory>
#include <mutex>

enum class HitsoundKind { NoteHit, NoteSideHit, HoldHit, HoldHeld, FlickHit, FieldHit, FieldHeld, Count };

struct HitsoundDescription {
    const char* key;
    const char* label;
    const char* defaultFile;
};

inline constexpr std::array<HitsoundDescription, static_cast<int>(HitsoundKind::Count)> kHitsoundDescriptions{{
    {"note_hit", "Note hit", "NoteHit.wav"},
    {"note_side_hit", "Note side hit", "NoteSideHit.wav"},
    {"hold_hit", "Hold hit", "NoteHit.wav"},
    {"hold_held", "Hold held", "NoteHeld.wav"},
    {"flick_hit", "Flick hit", "FlickHit.wav"},
    {"field_hit", "Field hit", "FieldHit.wav"},
    {"field_held", "Field held", "FieldHeld.wav"},
}};

struct HitsoundFile {
    QString name;
    QByteArray data;
    bool custom = false;
};

using HitsoundFiles = std::array<HitsoundFile, kHitsoundDescriptions.size()>;

struct AudioMixData {
    std::array<QByteArray, kHitsoundDescriptions.size()> sounds;
    std::mutex mutex;
    std::atomic<qreal> songVolume{1.0};
    std::atomic<qreal> effectsVolume{1.0};
};

struct HitsoundEvent {
    HitsoundKind kind;
    qint64 startMilliseconds;
    qint64 endMilliseconds;
    bool loop = false;
};

[[nodiscard]] QVector<HitsoundEvent> hitsoundEventsForChart(const ChartData& chart);

class HitsoundBank final : public QObject {
    Q_OBJECT

public:
    explicit HitsoundBank(const QAudioFormat& format, QObject* parent = nullptr);
    ~HitsoundBank() override;

    [[nodiscard]] const HitsoundFiles& files() const;
    [[nodiscard]] std::shared_ptr<AudioMixData> mix() const;
    [[nodiscard]] QByteArray samples(int index) const;
    [[nodiscard]] bool isDecoding() const;
    void load(const HitsoundFiles& files);
    void replace(int index, HitsoundFile file);
    void save() const;
    [[nodiscard]] static HitsoundFiles loadFiles();

signals:
    void soundReady(int index);
    void soundError(int index, const QString& message);

private:
    struct DecodeJob;
    void appendSamples(int index);

    QAudioFormat m_format;
    HitsoundFiles m_files;
    std::shared_ptr<AudioMixData> m_mix = std::make_shared<AudioMixData>();
    std::array<std::unique_ptr<DecodeJob>, kHitsoundDescriptions.size()> m_jobs;
};
