#include "core/project_document.h"

#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

#include <limits>

namespace infalsus {

namespace {

constexpr char kProjectMagic[] = "10NO";
constexpr quint16 kProjectVersion = 2;

[[nodiscard]] QJsonObject noteToJson(const ChartNote& note) {
    return {
        {QStringLiteral("id"), QString::number(note.id)},
        {QStringLiteral("group_id"), QString::number(note.groupId)},
        {QStringLiteral("auxiliary"), static_cast<qint64>(note.auxiliary)},
        {QStringLiteral("start_ms"), note.startMilliseconds},
        {QStringLiteral("end_ms"), note.endMilliseconds},
        {QStringLiteral("start_x"), note.startX},
        {QStringLiteral("end_x"), note.endX},
        {QStringLiteral("start_width"), note.startWidth},
        {QStringLiteral("end_width"), note.endWidth},
        {QStringLiteral("kind"), static_cast<int>(note.kind)},
        {QStringLiteral("side"), static_cast<int>(note.side)},
    };
}

[[nodiscard]] ChartNote noteFromJson(const QJsonObject& object) {
    ChartNote note;
    note.id = object.value(QStringLiteral("id")).toString().toULongLong();
    note.groupId = object.value(QStringLiteral("group_id")).toString().toULongLong();
    note.auxiliary = static_cast<std::uint32_t>(object.value(QStringLiteral("auxiliary")).toInteger());
    note.startMilliseconds = object.value(QStringLiteral("start_ms")).toInteger();
    note.endMilliseconds = object.value(QStringLiteral("end_ms")).toInteger();
    note.startX = object.value(QStringLiteral("start_x")).toDouble();
    note.endX = object.value(QStringLiteral("end_x")).toDouble();
    note.startWidth = object.value(QStringLiteral("start_width")).toDouble();
    note.endWidth = object.value(QStringLiteral("end_width")).toDouble();
    note.kind = static_cast<NoteKind>(object.value(QStringLiteral("kind")).toInt());
    note.side = static_cast<FloorSide>(object.value(QStringLiteral("side")).toInt());

    return note;
}

[[nodiscard]] QJsonObject timingPointToJson(const TimingPoint& point) {
    return {
        {QStringLiteral("time_ms"), point.timeMilliseconds},
        {QStringLiteral("bpm"), point.beatsPerMinute},
        {QStringLiteral("numerator"), point.timeSignatureNumerator},
        {QStringLiteral("denominator"), point.timeSignatureDenominator},
    };
}

[[nodiscard]] TimingPoint timingPointFromJson(const QJsonObject& object) {
    return {
        .timeMilliseconds = object.value(QStringLiteral("time_ms")).toInteger(),
        .beatsPerMinute = object.value(QStringLiteral("bpm")).toDouble(120.0),
        .timeSignatureNumerator = object.value(QStringLiteral("numerator")).toInt(4),
        .timeSignatureDenominator = object.value(QStringLiteral("denominator")).toInt(4),
    };
}

[[nodiscard]] QJsonObject laneEventToJson(const LaneEvent& event) {
    return {
        {QStringLiteral("time_ms"), event.timeMilliseconds},
        {QStringLiteral("lane"), event.lane},
        {QStringLiteral("enabled"), event.enabled},
    };
}

[[nodiscard]] LaneEvent laneEventFromJson(const QJsonObject& object) {
    return {
        .timeMilliseconds = object.value(QStringLiteral("time_ms")).toInteger(),
        .lane = object.value(QStringLiteral("lane")).toInt(),
        .enabled = object.value(QStringLiteral("enabled")).toBool(),
    };
}

[[nodiscard]] QJsonObject speedEventToJson(const SpeedEvent& event) {
    return {
        {QStringLiteral("time_ms"), event.timeMilliseconds},
        {QStringLiteral("speed"), event.speed},
    };
}

[[nodiscard]] SpeedEvent speedEventFromJson(const QJsonObject& object) {
    return {
        .timeMilliseconds = object.value(QStringLiteral("time_ms")).toInteger(),
        .speed = object.value(QStringLiteral("speed")).toDouble(1.0),
    };
}

[[nodiscard]] QJsonObject chartToJson(const DifficultyChart& difficulty) {
    QJsonArray notes;
    QJsonArray timingPoints;
    QJsonArray laneEvents;
    QJsonArray speedEvents;
    for (const ChartNote& note : difficulty.hitObjects.notes) {
        notes.append(noteToJson(note));
    }
    for (const TimingPoint& point : difficulty.timingPoints) {
        timingPoints.append(timingPointToJson(point));
    }
    for (const LaneEvent& event : difficulty.laneEvents) {
        laneEvents.append(laneEventToJson(event));
    }
    for (const SpeedEvent& event : difficulty.speedEvents) {
        speedEvents.append(speedEventToJson(event));
    }

    return {
        {QStringLiteral("name"), difficulty.hitObjects.name},
        {QStringLiteral("duration_ms"), difficulty.hitObjects.durationMilliseconds},
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("rating"), difficulty.metadata.rating},
        }},
        {QStringLiteral("notes"), notes},
        {QStringLiteral("timing_events"), timingPoints},
        {QStringLiteral("lane_events"), laneEvents},
        {QStringLiteral("speed_events"), speedEvents},
    };
}

[[nodiscard]] DifficultyChart chartFromJson(const Difficulty difficulty, const QJsonObject& object) {
    DifficultyChart chart;
    chart.difficulty = difficulty;
    const QJsonObject metadata = object.value(QStringLiteral("metadata")).toObject();
    chart.metadata = {
        .rating = metadata.value(QStringLiteral("rating")).toInt(1),
    };
    chart.hitObjects.name = object.value(QStringLiteral("name")).toString();
    chart.hitObjects.durationMilliseconds = object.value(QStringLiteral("duration_ms")).toInteger();
    for (const QJsonValue& value : object.value(QStringLiteral("notes")).toArray()) {
        chart.hitObjects.notes.append(noteFromJson(value.toObject()));
    }
    for (const QJsonValue& value : object.value(QStringLiteral("timing_events")).toArray()) {
        chart.timingPoints.append(timingPointFromJson(value.toObject()));
    }
    for (const QJsonValue& value : object.value(QStringLiteral("lane_events")).toArray()) {
        chart.laneEvents.append(laneEventFromJson(value.toObject()));
    }
    for (const QJsonValue& value : object.value(QStringLiteral("speed_events")).toArray()) {
        chart.speedEvents.append(speedEventFromJson(value.toObject()));
    }
    if (chart.timingPoints.isEmpty()) {
        chart.timingPoints.append(TimingPoint{});
    }

    return chart;
}

[[nodiscard]] QJsonObject projectToJson(const ChartProject& project) {
    QJsonObject difficulties;
    for (int index = 0; index < 4; ++index) {
        const Difficulty difficulty = difficultyForIndex(index);
        difficulties.insert(difficultyName(difficulty), chartToJson(project.difficulties.at(index)));
    }

    return {
        {QStringLiteral("format"), QStringLiteral("in_libertas_project")},
        {QStringLiteral("version"), kProjectVersion},
        {QStringLiteral("metadata"), QJsonObject{
            {QStringLiteral("artist_name"), project.metadata.artistName},
            {QStringLiteral("song_name"), project.metadata.songName},
            {QStringLiteral("chart_designer"), project.metadata.chartDesigner},
            {QStringLiteral("jacket_designer"), project.metadata.jacketDesigner},
            {QStringLiteral("preview_start_seconds"), project.metadata.previewStartSeconds},
            {QStringLiteral("preview_end_seconds"), project.metadata.previewEndSeconds},
            {QStringLiteral("character_identifier"), project.metadata.characterIdentifier},
            {QStringLiteral("gameplay_background"), project.metadata.gameplayBackground},
        }},
        {QStringLiteral("chart_id"), project.chartId},
        {QStringLiteral("assets"), QJsonObject{
            {QStringLiteral("song_file_name"), QFileInfo(project.songPath).fileName()},
            {QStringLiteral("jacket_file_name"), QFileInfo(project.jacketPath).fileName()},
        }},
        {QStringLiteral("difficulties"), difficulties},
    };
}

[[nodiscard]] bool projectFromJson(const QJsonObject& object, ChartProject* project, QString* error) {
    if (object.value(QStringLiteral("format")).toString() != QStringLiteral("in_libertas_project")) {
        *error = QStringLiteral("The project manifest has an unknown format.");
        return false;
    }
    if (object.value(QStringLiteral("version")).toInt() != kProjectVersion) {
        *error = QStringLiteral("The project manifest version is unsupported.");
        return false;
    }
    const QJsonObject metadata = object.value(QStringLiteral("metadata")).toObject();
    const QJsonObject assets = object.value(QStringLiteral("assets")).toObject();
    const QJsonObject difficulties = object.value(QStringLiteral("difficulties")).toObject();
    ChartProject loaded;
    loaded.metadata = {
        .artistName = metadata.value(QStringLiteral("artist_name")).toString(),
        .songName = metadata.value(QStringLiteral("song_name")).toString(),
        .chartDesigner = metadata.value(QStringLiteral("chart_designer")).toString(),
        .jacketDesigner = metadata.value(QStringLiteral("jacket_designer")).toString(),
        .previewStartSeconds = metadata.value(QStringLiteral("preview_start_seconds")).toDouble(),
        .previewEndSeconds = metadata.value(QStringLiteral("preview_end_seconds")).toDouble(),
        .characterIdentifier = metadata.value(QStringLiteral("character_identifier")).toString(),
        .gameplayBackground = metadata.value(QStringLiteral("gameplay_background")).toString(QStringLiteral("0")),
    };
    loaded.chartId = object.value(QStringLiteral("chart_id")).toString();
    loaded.songPath = assets.value(QStringLiteral("song_file_name")).toString();
    loaded.jacketPath = assets.value(QStringLiteral("jacket_file_name")).toString();
    for (int index = 0; index < 4; ++index) {
        const Difficulty difficulty = difficultyForIndex(index);
        loaded.difficulties.at(index) = chartFromJson(difficulty, difficulties.value(difficultyName(difficulty)).toObject());
    }
    *project = std::move(loaded);

    return true;
}

} // namespace

bool ProjectDocument::LoadResult::succeeded() const {
    return error.isEmpty();
}

bool ProjectDocument::save(const QString& filePath, const ChartProject& project,
    const QByteArray& songData, const QByteArray& jacketData, QString* error) {
    if (songData.isEmpty()) {
        *error = QStringLiteral("Could not read the project song.");
        return false;
    }
    const QByteArray manifest = QJsonDocument(projectToJson(project)).toJson(QJsonDocument::Compact);
    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        *error = QStringLiteral("Could not open the project for writing.");
        return false;
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(kProjectMagic, 4);
    stream << kProjectVersion << static_cast<quint32>(manifest.size())
           << static_cast<quint64>(songData.size()) << static_cast<quint64>(jacketData.size());
    stream.writeRawData(manifest.constData(), manifest.size());
    stream.writeRawData(songData.constData(), songData.size());
    stream.writeRawData(jacketData.constData(), jacketData.size());
    if (stream.status() != QDataStream::Ok || !file.commit()) {
        *error = QStringLiteral("Could not finish writing the project.");
        return false;
    }

    return true;
}

bool ProjectDocument::save(const QString& filePath, const ChartProject& project, QString* error) {
    QFile songFile(project.songPath);
    if (!songFile.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("Could not read the project song.");
        return false;
    }
    const QByteArray songData = songFile.readAll();
    QByteArray jacketData;
    if (!project.jacketPath.isEmpty()) {
        QFile jacketFile(project.jacketPath);
        if (!jacketFile.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Could not read the project jacket.");
            return false;
        }
        jacketData = jacketFile.readAll();
    }
    return save(filePath, project, songData, jacketData, error);
}

ProjectDocument::LoadResult ProjectDocument::load(const QString& filePath) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {.error = QStringLiteral("Could not open the project.")};
    }
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    char magic[4]{};
    quint16 version = 0;
    quint32 manifestSize = 0;
    quint64 songSize = 0;
    quint64 jacketSize = 0;
    if (stream.readRawData(magic, 4) != 4 || QByteArray(magic, 4) != QByteArray(kProjectMagic, 4)) {
        return {.error = QStringLiteral("The file is not a .10no project.")};
    }
    stream >> version >> manifestSize >> songSize >> jacketSize;
    constexpr quint64 kMaximumManifestSize = 32ULL * 1024ULL * 1024ULL;
    if (version != kProjectVersion || manifestSize > kMaximumManifestSize
        || songSize > static_cast<quint64>(std::numeric_limits<qsizetype>::max())
        || jacketSize > static_cast<quint64>(std::numeric_limits<qsizetype>::max())) {
        return {.error = QStringLiteral("The .10no project header is invalid.")};
    }
    QByteArray manifest(static_cast<qsizetype>(manifestSize), Qt::Uninitialized);
    QByteArray songData(static_cast<qsizetype>(songSize), Qt::Uninitialized);
    QByteArray jacketData(static_cast<qsizetype>(jacketSize), Qt::Uninitialized);
    if (stream.readRawData(manifest.data(), manifest.size()) != manifest.size()
        || stream.readRawData(songData.data(), songData.size()) != songData.size()
        || stream.readRawData(jacketData.data(), jacketData.size()) != jacketData.size()) {
        return {.error = QStringLiteral("The .10no project is truncated.")};
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(manifest, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        return {.error = QStringLiteral("The .10no project manifest is invalid.")};
    }
    LoadResult result;
    if (!projectFromJson(document.object(), &result.project, &result.error)) {
        return result;
    }
    result.songFileName = result.project.songPath;
    result.jacketFileName = result.project.jacketPath;
    result.songData = std::move(songData);
    result.jacketData = std::move(jacketData);

    return result;
}

} // namespace infalsus
