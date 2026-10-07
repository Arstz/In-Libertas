using System;
using System.Collections.Generic;
using System.IO;
using System.Security.Cryptography;
using System.Text.Json;

namespace InFalsusCustomSongHook;

internal sealed class ExportOwnership {
    public int Version { get; set; } = 1;
    public string ChartId { get; set; }
    public int ConverterRevision { get; set; } = ProjectExport.kConverterRevision;
    public string SourceSha256 { get; set; }
    public Dictionary<string, string> Files { get; set; } = new(StringComparer.OrdinalIgnoreCase);
}

internal static class ProjectExport {
    internal const int kConverterRevision = 2;
    internal const string kOwnershipFile = ".inlibertas-export.json";
    internal static readonly JsonSerializerOptions JsonOptions = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };

    internal static void Write(string directory, ProjectPackage package, string sourceHash) {
        Dictionary<string, byte[]> output = new(StringComparer.OrdinalIgnoreCase) {
            ["audio.ogg"] = package.Audio, ["jacketLarge.png"] = package.Jacket, ["jacketSmall.png"] = JacketThumbnail.Create(package.Jacket),
        };
        object[] difficulties = new object[4];
        for (int index = 0; index < 4; index++) {
            JsonElement chart = package.Difficulties[index];
            if (chart.GetProperty("notes").GetArrayLength() == 0) continue;
            int rating = chart.GetProperty("metadata").GetProperty("rating").GetInt32();
            if (rating < 1) throw new InvalidDataException("Difficulty rating must be positive.");
            string stem = package.ChartId + index;
            output[stem + ".spc"] = ProjectChartEncoder.Encode(chart, stem + ".spc");
            difficulties[index] = new { externalChartId = stem, rating, levelSectionIndicator = rating.ToString(System.Globalization.CultureInfo.InvariantCulture) };
        }
        JsonElement metadata = package.Metadata;
        output["config.json"] = JsonSerializer.SerializeToUtf8Bytes(new {
            enabled = true, templateSongId = 2, chartId = package.ChartId, baseName = package.ChartId,
            audioFile = "audio.ogg", jacketLargeFile = "jacketLarge.png", jacketSmallFile = "jacketSmall.png", enableLoosePngJackets = true,
            songTitle = ProjectReader.Text(metadata, "song_name"), artist = ProjectReader.Text(metadata, "artist_name"),
            chartDesigner = ProjectReader.Text(metadata, "chart_designer"), jacketDesigner = ProjectReader.Text(metadata, "jacket_designer"),
            previewStartSeconds = ProjectReader.Number(metadata, "preview_start_seconds"), previewEndSeconds = ProjectReader.Number(metadata, "preview_end_seconds"),
            characterIdentifier = OptionalText(metadata, "character_identifier"), gameplayBackground = OptionalText(metadata, "gameplay_background"), difficulties,
        }, JsonOptions);
        ExportOwnership ownership = new() { ChartId = package.ChartId, SourceSha256 = sourceHash };
        Directory.CreateDirectory(directory);
        foreach (var entry in output) {
            File.WriteAllBytes(Path.Combine(directory, entry.Key), entry.Value);
            ownership.Files.Add(entry.Key, Hash(entry.Value));
        }
        File.WriteAllBytes(Path.Combine(directory, kOwnershipFile), JsonSerializer.SerializeToUtf8Bytes(ownership, JsonOptions));
        Verify(directory, package.ChartId);
    }

    internal static ExportOwnership Verify(string directory, string chartId) {
        EnsureOrdinary(directory);
        string marker = Path.Combine(directory, kOwnershipFile);
        EnsureOrdinary(marker);
        if (new FileInfo(marker).Length > ProjectReader.kMaximumManifestBytes) throw new InvalidDataException("Export marker is too large.");
        ExportOwnership ownership = JsonSerializer.Deserialize<ExportOwnership>(File.ReadAllBytes(marker), JsonOptions);
        if (ownership is null || ownership.Version != 1 || ownership.ConverterRevision < 1 || ownership.ChartId != chartId
            || ownership.Files is null || ownership.Files.Count < 5 || ownership.Files.Count > 8)
            throw new InvalidDataException("Folder is not a matching managed export.");
        Dictionary<string, string> inventory = new(StringComparer.OrdinalIgnoreCase);
        foreach (var entry in ownership.Files)
            if (!inventory.TryAdd(entry.Key, entry.Value)) throw new InvalidDataException("Duplicate export inventory filename.");
        ownership.Files = inventory;
        HashSet<string> names = new(StringComparer.OrdinalIgnoreCase) { "audio.ogg", "jacketLarge.png", "jacketSmall.png", "config.json" };
        for (int index = 0; index < 4; index++) names.Add(chartId + index + ".spc");
        foreach (var entry in ownership.Files) {
            if (!names.Contains(entry.Key) || entry.Value is null || entry.Value.Length != 64)
                throw new InvalidDataException("Invalid export inventory.");
            string path = Path.Combine(directory, entry.Key);
            EnsureOrdinary(path);
            if (!string.Equals(HashFile(path), entry.Value, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Export inventory mismatch: " + entry.Key);
        }
        foreach (string required in new[] { "audio.ogg", "jacketLarge.png", "jacketSmall.png", "config.json" })
            if (!ownership.Files.ContainsKey(required)) throw new InvalidDataException("Incomplete export inventory.");
        foreach (string path in Directory.EnumerateFileSystemEntries(directory)) {
            string name = Path.GetFileName(path);
            if (name != kOwnershipFile && !ownership.Files.ContainsKey(name))
                throw new InvalidDataException("Managed folder contains unrelated files: " + name);
        }
        using JsonDocument config = JsonDocument.Parse(File.ReadAllBytes(Path.Combine(directory, "config.json")));
        if (ProjectReader.Text(config.RootElement, "chartId") != chartId || ProjectReader.Text(config.RootElement, "baseName") != chartId)
            throw new InvalidDataException("Export configuration identity mismatch.");
        JsonElement settings = config.RootElement;
        if (ProjectReader.Text(settings, "audioFile") != "audio.ogg"
            || ProjectReader.Text(settings, "jacketLargeFile") != "jacketLarge.png"
            || ProjectReader.Text(settings, "jacketSmallFile") != "jacketSmall.png"
            || !settings.GetProperty("enabled").GetBoolean() || !settings.GetProperty("enableLoosePngJackets").GetBoolean())
            throw new InvalidDataException("Invalid managed media configuration.");
        double previewStart = ProjectReader.Number(settings, "previewStartSeconds"), previewEnd = ProjectReader.Number(settings, "previewEndSeconds");
        if (previewStart < 0 || previewStart >= previewEnd) throw new InvalidDataException("Invalid export preview.");
        JsonElement difficulties = settings.GetProperty("difficulties");
        if (difficulties.GetArrayLength() != 4) throw new InvalidDataException("Invalid export difficulty slots.");
        int charts = 0;
        for (int index = 0; index < 4; index++) {
            string stem = chartId + index;
            bool present = ownership.Files.ContainsKey(stem + ".spc");
            JsonElement difficulty = difficulties[index];
            if (difficulty.ValueKind == JsonValueKind.Null) {
                if (present) throw new InvalidDataException("Unconfigured chart payload.");
                continue;
            }
            if (!present || ProjectReader.Text(difficulty, "externalChartId") != stem
                || difficulty.GetProperty("rating").GetInt32() < 1)
                throw new InvalidDataException("Invalid export difficulty configuration.");
            VerifyChart(Path.Combine(directory, stem + ".spc"));
            charts++;
        }
        if (charts == 0) throw new InvalidDataException("No exported charts.");
        byte[] jacket = File.ReadAllBytes(Path.Combine(directory, "jacketLarge.png"));
        byte[] smallJacket = File.ReadAllBytes(Path.Combine(directory, "jacketSmall.png"));
        if (ownership.ConverterRevision == 1) {
            if (!Hash(jacket).Equals(Hash(smallJacket), StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("Managed jacket files differ.");
        } else {
            ProjectMedia.ValidatePng(smallJacket);
            DecodedPng thumbnail = PngDecoder.Decode(smallJacket);
            if (thumbnail.Width != JacketThumbnail.kExtent || thumbnail.Height != JacketThumbnail.kExtent)
                throw new InvalidDataException("Managed small jacket must be 256 by 256 pixels.");
        }
        ProjectMedia.Validate(File.ReadAllBytes(Path.Combine(directory, "audio.ogg")), jacket);

        return ownership;
    }

    private static void VerifyChart(string path) {
        using FileStream file = new(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        using BinaryReader reader = new(file);
        if (file.Length < 28 || reader.ReadUInt32() != 827343689 || reader.ReadUInt16() != 1
            || reader.ReadUInt16() != 28 || reader.ReadUInt16() != 80 || reader.ReadUInt16() != 32)
            throw new InvalidDataException("Invalid generated SPC header.");
        uint notes = reader.ReadUInt32(), events = reader.ReadUInt32();
        float bpm = reader.ReadSingle(), meter = reader.ReadSingle();
        if (notes == 0 || events == 0 || file.Length != 28L + notes * 80L + events * 32L
            || !float.IsFinite(bpm) || bpm <= 0 || !float.IsFinite(meter) || meter < 1 || meter > 32)
            throw new InvalidDataException("Invalid generated SPC payload.");
    }

    internal static void EnsureOrdinary(string path) {
        if ((File.GetAttributes(path) & FileAttributes.ReparsePoint) != 0)
            throw new IOException("Refusing a symbolic link or junction: " + path);
    }

    internal static string Hash(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();
    internal static string HashFile(string path) {
        using FileStream file = new(path, FileMode.Open, FileAccess.Read, FileShare.Read);

        return HashStream(file);
    }

    internal static string HashStream(Stream stream) {
        using SHA256 hash = SHA256.Create();

        return Convert.ToHexString(hash.ComputeHash(stream)).ToLowerInvariant();
    }

    private static string OptionalText(JsonElement element, string name) {
        string value = ProjectReader.Text(element, name);

        return string.IsNullOrEmpty(value) ? null : value;
    }
}
