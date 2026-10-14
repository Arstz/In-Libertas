#include "gui/state/hitsound_bank.h"

#include <QtCore/QBuffer>
#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QMap>
#include <QtCore/QSettings>
#include <QtMultimedia/QAudioDecoder>

#include <algorithm>

namespace {

[[nodiscard]] QString settingsGroup(const int index) {
    return QStringLiteral("hitsounds/%1").arg(QString::fromLatin1(kHitsoundDescriptions[index].key));
}

} // namespace

QVector<HitsoundEvent> hitsoundEventsForChart(const ChartData& chart) {
    struct FieldDuration {
        qint64 start;
        qint64 end;
    };
    QVector<HitsoundEvent> events;
    QMap<std::uint64_t, FieldDuration> fields;

    for (const ChartNote& note : chart.notes) {
        switch (note.kind) {
        case NoteKind::Tap:
            events.append({note.side == FloorSide::LeftOuter || note.side == FloorSide::RightOuter
                ? HitsoundKind::NoteSideHit : HitsoundKind::NoteHit, note.startMilliseconds, note.startMilliseconds});
            break;
        case NoteKind::Hold:
            events.append({HitsoundKind::HoldHit, note.startMilliseconds, note.startMilliseconds});
            events.append({HitsoundKind::HoldHeld, note.startMilliseconds, note.endMilliseconds, true});
            break;
        case NoteKind::Flick:
            events.append({HitsoundKind::FlickHit, note.startMilliseconds, note.startMilliseconds});
            break;
        case NoteKind::Sky: {
            auto field = fields.find(note.groupId);
            if (field == fields.end()) {
                fields.insert(note.groupId, {note.startMilliseconds, note.endMilliseconds});
            } else {
                field->start = std::min(field->start, note.startMilliseconds);
                field->end = std::max(field->end, note.endMilliseconds);
            }
            break;
        }
        case NoteKind::Unknown:
            break;
        }
    }
    for (const FieldDuration& field : fields) {
        events.append({HitsoundKind::FieldHit, field.start, field.start});
        events.append({HitsoundKind::FieldHeld, field.start, field.end, true});
    }
    std::stable_sort(events.begin(), events.end(), [](const HitsoundEvent& first, const HitsoundEvent& second) {
        return first.startMilliseconds < second.startMilliseconds;
    });

    return events;
}

struct HitsoundBank::DecodeJob {
    HitsoundFile file;
    QByteArray samples;
    QBuffer buffer;
    QAudioDecoder decoder;
    bool active = true;
};

HitsoundBank::HitsoundBank(const QAudioFormat& format, QObject* parent)
    : QObject(parent), m_format(format) {
}

HitsoundBank::~HitsoundBank() = default;

const HitsoundFiles& HitsoundBank::files() const {
    return m_files;
}

std::shared_ptr<AudioMixData> HitsoundBank::mix() const {
    return m_mix;
}

QByteArray HitsoundBank::samples(const int index) const {
    const std::lock_guard lock(m_mix->mutex);

    return m_mix->sounds.at(index);
}

bool HitsoundBank::isDecoding() const {
    return std::any_of(m_jobs.begin(), m_jobs.end(), [](const auto& job) { return job && job->active; });
}

void HitsoundBank::load(const HitsoundFiles& files) {
    m_files = files;
    for (int index = 0; index < static_cast<int>(files.size()); ++index) {
        replace(index, files[index]);
    }
}

void HitsoundBank::replace(const int index, HitsoundFile file) {
    m_jobs[index] = std::make_unique<DecodeJob>();
    DecodeJob& job = *m_jobs[index];
    job.file = std::move(file);
    job.buffer.setData(job.file.data);
    job.buffer.open(QIODevice::ReadOnly);
    job.decoder.setAudioFormat(m_format);
    job.decoder.setSourceDevice(&job.buffer);
    connect(&job.decoder, &QAudioDecoder::bufferReady, this, [this, index] { appendSamples(index); });
    connect(&job.decoder, &QAudioDecoder::finished, this, [this, index] {
        appendSamples(index);
        DecodeJob& finished = *m_jobs[index];
        if (!finished.active) {
            return;
        }
        finished.active = false;
        if (finished.samples.isEmpty()) {
            emit soundError(index, QStringLiteral("The file contains no decodable audio."));
            return;
        }
        m_files[index] = finished.file;
        {
            const std::lock_guard lock(m_mix->mutex);
            m_mix->sounds[index] = finished.samples;
        }
        emit soundReady(index);
    });
    connect(&job.decoder, qOverload<QAudioDecoder::Error>(&QAudioDecoder::error), this,
        [this, index](QAudioDecoder::Error) {
            m_jobs[index]->active = false;
            emit soundError(index, m_jobs[index]->decoder.errorString());
        });
    job.decoder.start();
}

void HitsoundBank::save() const {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    for (int index = 0; index < static_cast<int>(m_files.size()); ++index) {
        const HitsoundFile& file = m_files[index];
        const QString group = settingsGroup(index);
        if (file.custom) {
            settings.setValue(group + QStringLiteral("/name"), file.name);
            settings.setValue(group + QStringLiteral("/data"), file.data);
        } else {
            settings.remove(group);
        }
    }
}

HitsoundFiles HitsoundBank::loadFiles() {
    QSettings settings(QStringLiteral("InLibertas"), QStringLiteral("In Libertas"));
    const QDir assets(QCoreApplication::applicationDirPath() + QStringLiteral("/assets"));
    HitsoundFiles files;

    for (int index = 0; index < static_cast<int>(files.size()); ++index) {
        const QString group = settingsGroup(index);
        const QString defaultName = QString::fromLatin1(kHitsoundDescriptions[index].defaultFile);
        HitsoundFile& file = files[index];
        file.data = settings.value(group + QStringLiteral("/data")).toByteArray();
        file.custom = !file.data.isEmpty();
        file.name = file.custom ? settings.value(group + QStringLiteral("/name"), defaultName).toString() : defaultName;
        if (!file.custom) {
            QFile source(assets.filePath(defaultName));
            if (source.open(QIODevice::ReadOnly)) {
                file.data = source.readAll();
            }
        }
    }

    return files;
}

void HitsoundBank::appendSamples(const int index) {
    DecodeJob& job = *m_jobs[index];

    while (job.active && job.decoder.bufferAvailable()) {
        const QAudioBuffer buffer = job.decoder.read();
        if (!buffer.isValid()) {
            continue;
        }
        if (buffer.format() != m_format) {
            job.active = false;
            emit soundError(index, QStringLiteral("The decoder returned an unsupported audio format."));
            return;
        }
        job.samples.append(buffer.constData<char>(), buffer.byteCount());
    }
}
