using System;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Serialization;
using MelonLoader;

namespace InFalsusCustomSongHook;

internal sealed class CustomSongConfig
{
    private static readonly System.Collections.Generic.HashSet<string> ReportedFolderErrors = new(StringComparer.OrdinalIgnoreCase);
    public bool Enabled { get; set; }
    // A library is a set of real PackData entries.  Each collection becomes a
    // separate arc on the game's pack-select screen and can contain any
    // number of songs.
    public CustomSongCollection[] Collections { get; set; }
    // The shipped SongData table has an unused, non-zero slot at index 66.
    public ushort SongId { get; set; } = 66;
    public ushort TemplateSongId { get; set; } = 2;
    // Unique custom-song/chart identity. It need not match the filename of
    // the external decoded chart payload.
    public string ChartId { get; set; } = "custom0";
    // When set, do not install a SongData entry. Instead, replace this
    // already-mounted stock chart at the ICP1 parser boundary. The carrier
    // keeps its stock selector identity, audio, jackets, and result data.
    public string CarrierChartId { get; set; }
    public string ExternalChartId { get; set; }
    // When present, this song occupies multiple native difficulty slots. Each
    // array index is a slot: 0=Minimal through 3=Forbidden. A null entry is
    // unavailable. The legacy top-level chart fields remain supported for a
    // one-difficulty folder.
    public CustomSongDifficulty[] Difficulties { get; set; }

    // ICP1 protects each note with rolling state seeded by the parser's chart
    // key. An SPC exported by In Libertas is seeded with its output filename,
    // so null uses ExternalChartId. Set this explicitly only for a payload
    // encoded under a different key (for example, an untouched stock payload).
    public string ParserChartId { get; set; }
    public byte Difficulty { get; set; } = 1;
    public int Rating { get; set; } = 1;
    public string LevelSectionIndicator { get; set; }
    public string PackSlug { get; set; }
    // Null derives the song identity from a conventional chart ID: custom0 at
    // Minimal becomes custom, so GameScene derives custom0.spc.
    public string BaseName { get; set; }
    // A normal FMOD-supported file, stored in this song's content directory.
    // It is opened directly; this is not a StreamingAssets/sam payload.
    public string AudioFile { get; set; } = "audio.ogg";
    // Loose PNGs are cloned into template materials. The native resolver
    // companion returns completed material operations for marker references,
    // so stock consumers retain their normal ownership path without a
    // provider, catalog, or AssetBundle.
    public string JacketLargeFile { get; set; } = "jacketLarge.png";
    public string JacketSmallFile { get; set; } = "jacketSmall.png";
    // Uses the hook's native jacket resolver companion. Raw config defaults
    // to false; the chart-folder loader enables it only after validating both
    // declared PNG files.
    public bool EnableLoosePngJackets { get; set; }
    public string SongTitle { get; set; } = "Custom Song";
    public string Artist { get; set; } = "Custom Artist";
    public string ChartDesigner { get; set; } = "Custom chart";
    public string JacketDesigner { get; set; } = "Custom jacket";
    public float? PreviewStartSeconds { get; set; }
    public float? PreviewEndSeconds { get; set; }
    // These are enum member names from the game's generated API, for example
    // values visible in its existing SongInfo records. Null retains the
    // template value.
    public string CharacterIdentifier { get; set; }
    public string GameplayBackground { get; set; }
    // This is the logical key carried by SongChartInfo and passed to the
    // chart parser. ExternalChartId is only the on-disk payload stem.
    public string ChartKey => ChartId + ".spc";
    public string ExternalChartStem => string.IsNullOrWhiteSpace(ExternalChartId) ? ChartId : ExternalChartId;
    public bool IsCarrierChartOverride => !string.IsNullOrWhiteSpace(CarrierChartId);
    public bool IsLibrary => Collections is { Length: > 0 };
    public bool HasDifficultySpread => Difficulties is { Length: > 0 };
    public string ActiveChartKey => (IsCarrierChartOverride ? CarrierChartId : ChartId) + ".spc";

    [JsonIgnore]
    internal string ContentDirectory { get; set; }

    internal CustomSongConfig[] ExpandDifficulties()
    {
        if (!HasDifficultySpread) return new[] { this };

        var entries = new System.Collections.Generic.List<CustomSongConfig>();
        for (int index = 0; index < Difficulties.Length; index++)
        {
            CustomSongDifficulty difficulty = Difficulties[index];
            if (difficulty is null) continue;
            entries.Add(new CustomSongConfig
            {
                Enabled = Enabled,
                SongId = SongId,
                TemplateSongId = TemplateSongId,
                ChartId = ChartId + index,
                ExternalChartId = string.IsNullOrWhiteSpace(difficulty.ExternalChartId)
                    ? ChartId + index : difficulty.ExternalChartId,
                ParserChartId = string.IsNullOrWhiteSpace(difficulty.ParserChartId)
                    ? ParserChartId : difficulty.ParserChartId,
                Difficulty = (byte)(1 << index),
                Rating = difficulty.Rating,
                LevelSectionIndicator = difficulty.LevelSectionIndicator,
                PackSlug = PackSlug,
                BaseName = string.IsNullOrWhiteSpace(BaseName) ? ChartId : BaseName,
                AudioFile = AudioFile,
                JacketLargeFile = JacketLargeFile,
                JacketSmallFile = JacketSmallFile,
                EnableLoosePngJackets = EnableLoosePngJackets,
                SongTitle = SongTitle,
                Artist = Artist,
                ChartDesigner = string.IsNullOrWhiteSpace(difficulty.ChartDesigner) ? ChartDesigner : difficulty.ChartDesigner,
                JacketDesigner = string.IsNullOrWhiteSpace(difficulty.JacketDesigner) ? JacketDesigner : difficulty.JacketDesigner,
                PreviewStartSeconds = PreviewStartSeconds,
                PreviewEndSeconds = PreviewEndSeconds,
                CharacterIdentifier = CharacterIdentifier,
                GameplayBackground = GameplayBackground,
                ContentDirectory = ContentDirectory,
            });
        }
        return entries.ToArray();
    }

    private static string GameRoot => MelonLoader.Utils.MelonEnvironment.GameRootDirectory;
    public static string CustomChartsRoot => Path.Combine(GameRoot, "CustomCharts");

    public string AudioPath => Path.Combine(ContentDirectory ?? throw new InvalidOperationException(
        "A custom song must be loaded from a chart folder."), AudioFile);

    public string JacketLargePath => GetOptionalContentPath(JacketLargeFile);
    public string JacketSmallPath => GetOptionalContentPath(JacketSmallFile);

    public static bool TryLoad(out CustomSongConfig config, out string message)
    {
        return TryLoadFolderLibrary(out config, out message);
    }

    private static bool TryLoadFolderLibrary(out CustomSongConfig config, out string message)
    {
        config = null;
        try
        {
            var songs = new System.Collections.Generic.List<CustomSongConfig>();
            foreach (string directory in Directory.EnumerateDirectories(CustomChartsRoot).OrderBy(path => path, StringComparer.OrdinalIgnoreCase))
            {
                try
                {
                    if (Path.GetFileName(directory).StartsWith(".inlibertas-", StringComparison.OrdinalIgnoreCase)) continue;
                    ProjectExport.EnsureOrdinary(directory);
                    string folderConfigPath = Path.Combine(directory, "config.json");
                    if (!File.Exists(folderConfigPath)) continue;

                    CustomSongConfig folderConfig = JsonSerializer.Deserialize<CustomSongConfig>(
                        File.ReadAllText(folderConfigPath),
                        new JsonSerializerOptions { PropertyNameCaseInsensitive = true });
                    if (folderConfig is null)
                        throw new InvalidDataException("configuration is empty: " + folderConfigPath);
                    if (!folderConfig.Enabled) continue;

                    CustomSongConfig song = folderConfig;
                    if (folderConfig.IsLibrary)
                    {
                        if (folderConfig.Collections.Length != 1 || folderConfig.Collections[0]?.Songs?.Length != 1)
                            throw new InvalidDataException("Folder config must contain one collection and one song.");
                        song = folderConfig.Collections[0].Songs[0]
                            ?? throw new InvalidDataException("Folder song cannot be null.");
                    }
                    if (song.IsLibrary || song.IsCarrierChartOverride)
                        throw new InvalidDataException("Folder config must describe one injected song.");
                    song.EnableLoosePngJackets = true;
                    song.ContentDirectory = directory;
                    if (!ValidateLibrarySong(song, out string songError))
                        throw new InvalidDataException(songError);
                    foreach (CustomSongConfig entry in song.ExpandDifficulties())
                        if (!File.Exists(Path.Combine(directory, entry.ExternalChartStem + ".spc"))
                            && !File.Exists(Path.Combine(directory, entry.ExternalChartStem + ".sam")))
                            throw new InvalidDataException("Chart payload not found: " + entry.ExternalChartStem);
                    if (songs.Any(existing => string.Equals(existing.BaseName ?? existing.ChartId, song.BaseName ?? song.ChartId, StringComparison.OrdinalIgnoreCase)
                        || existing.ExpandDifficulties().Any(entry => song.ExpandDifficulties().Any(candidate =>
                            string.Equals(entry.ChartId, candidate.ChartId, StringComparison.OrdinalIgnoreCase)))))
                        throw new InvalidDataException("Duplicate custom song/chart identity.");
                    songs.Add(song);
                }
                catch (Exception exception)
                {
                    string warning = directory + ": " + exception.Message;
                    if (ReportedFolderErrors.Add(warning))
                        CustomSongMod.Log.Warning("[CustomSong] skipped chart folder " + warning);
                }
            }

            if (songs.Count == 0)
            {
                message = "no enabled chart folders with config.json in " + CustomChartsRoot;
                return false;
            }

            config = new CustomSongConfig
            {
                Enabled = true,
                Collections = new[]
                {
                    new CustomSongCollection
                    {
                        PackId = CustomSongCollection.kDefaultPackId,
                        Slug = "custom-charts",
                        Title = "Custom Charts",
                        StylePackSlug = "act-4",
                        Songs = songs.ToArray(),
                    },
                },
            };
            if (!ValidateLibrary(config, out message)) return false;
            message = "ok (folder custom-chart library: " + songs.Count + " songs)";
            return true;
        }
        catch (Exception exception)
        {
            config = null;
            message = "folder configuration parse failed: " + exception.Message;
            return false;
        }
    }

    private static bool IsFileNameStem(string value) =>
        !string.IsNullOrWhiteSpace(value) &&
        value.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 &&
        !value.Contains('/') && !value.Contains('\\');

    private static bool IsOptionalFileName(string value) =>
        string.IsNullOrWhiteSpace(value) ||
        (value.IndexOfAny(Path.GetInvalidFileNameChars()) < 0 &&
         !value.Contains('/') && !value.Contains('\\'));

    private string GetOptionalContentPath(string fileName) =>
        string.IsNullOrWhiteSpace(fileName)
            ? null
            : Path.Combine(ContentDirectory ?? throw new InvalidOperationException(
                "A custom jacket must be loaded from a chart folder."), fileName);

    private static bool ValidateLibrary(CustomSongConfig config, out string message)
    {
        var seenPackIds = new System.Collections.Generic.HashSet<ushort>();
        var seenSlugs = new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var seenCharts = new System.Collections.Generic.HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (CustomSongCollection collection in config.Collections)
        {
            if (collection is null || collection.PackId == 0 ||
                string.IsNullOrWhiteSpace(collection.Slug) ||
                string.IsNullOrWhiteSpace(collection.Title) ||
                collection.Songs is not { Length: > 0 })
            {
                message = "each collection needs a non-zero packId, slug, title, and at least one song";
                return false;
            }
            if (!seenPackIds.Add(collection.PackId) || !seenSlugs.Add(collection.Slug))
            {
                message = "collection packId and slug values must be unique";
                return false;
            }
            foreach (CustomSongConfig song in collection.Songs)
            {
                if (song is null)
                {
                    message = "collection song entries cannot be null";
                    return false;
                }
                if (song.IsCarrierChartOverride)
                {
                    message = "carrierChartId cannot appear inside a collection";
                    return false;
                }
                if (!ValidateLibrarySong(song, out message))
                    return false;
                foreach (CustomSongConfig entry in song.ExpandDifficulties())
                {
                    if (!seenCharts.Add(entry.ChartId))
                    {
                        message = "every library chartId must be unique";
                        return false;
                    }
                }
            }
        }
        message = "ok";
        return true;
    }

    private static bool ValidateLibrarySong(CustomSongConfig song, out string message)
    {
        if (song.HasDifficultySpread)
        {
            if (song.Difficulties.Length > 4 || song.Difficulties.All(difficulty => difficulty is null))
            {
                message = "library difficulties must contain one to four non-null entries at indexes 0 through 3";
                return false;
            }
            if (!IsFileNameStem(song.ChartId))
            {
                message = "a multi-difficulty library song needs a chartId base stem";
                return false;
            }
            foreach (CustomSongConfig entry in song.ExpandDifficulties())
            {
                if (!ValidateLibrarySong(entry, out message)) return false;
            }
            message = "ok";
            return true;
        }
        if (!IsFileNameStem(song.ChartId) || !IsFileNameStem(song.ExternalChartStem))
        {
            message = "each library song needs chartId and an optional externalChartId file-name stem";
            return false;
        }
        if (song.ParserChartId is not null && !IsFileNameStem(song.ParserChartId))
        {
            message = "library parserChartId must be null or a file-name stem";
            return false;
        }
        if (song.Difficulty is not (1 or 2 or 4 or 8))
        {
            message = "library difficulty must be one of 1, 2, 4, or 8";
            return false;
        }
        if (string.IsNullOrWhiteSpace(song.AudioFile) || song.AudioFile.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 ||
            song.AudioFile.Contains('/') || song.AudioFile.Contains('\\'))
        {
            message = "library audioFile must be a file name in its chart folder";
            return false;
        }
        if (!File.Exists(song.AudioPath))
        {
            message = "library audio file not found: " + song.AudioPath;
            return false;
        }
        if (song.EnableLoosePngJackets && !ValidateJacketFiles(song, out message)) return false;
        message = "ok";
        return true;
    }

    private static bool ValidateJacketFiles(CustomSongConfig song, out string message)
    {
        if (!IsOptionalFileName(song.JacketLargeFile) || !IsOptionalFileName(song.JacketSmallFile))
        {
            message = "jacketLargeFile and jacketSmallFile must be file names located in the chart folder";
            return false;
        }
        if (!File.Exists(song.JacketLargePath) || !File.Exists(song.JacketSmallPath))
        {
            message = "jacket PNG file not found: " + song.JacketLargePath + " or " + song.JacketSmallPath;
            return false;
        }
        message = "ok";
        return true;
    }
}

internal sealed class CustomSongDifficulty
{
    public string ExternalChartId { get; set; }
    public string ParserChartId { get; set; }
    public int Rating { get; set; } = 1;
    public string LevelSectionIndicator { get; set; }
    public string ChartDesigner { get; set; }
    public string JacketDesigner { get; set; }
}

internal sealed class CustomSongCollection
{
    internal const ushort kDefaultPackId = 100;

    // Installation maps configured IDs to their appended PackInfo positions.
    public ushort PackId { get; set; } = kDefaultPackId;
    public string Slug { get; set; } = "custom";
    public string Title { get; set; } = "Custom Charts";
    // Reuse a stock collection's visual theme until custom pack art is added.
    public string StylePackSlug { get; set; } = "act-4";
    public CustomSongConfig[] Songs { get; set; }
}
