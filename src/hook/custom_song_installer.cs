using System;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.InteropTypes;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using Il2Cppifapp.Game;
using Il2Cppifapp.Game.Common;
using Il2Cppifapp.Game.Data;
using Il2Cppifapp.Game.Scenes;
using UnityEngine;

namespace InFalsusCustomSongHook;

/// <summary>
/// Occupies one of the game's preallocated, invalid SongInfo slots before
/// SongSelectScene builds its active list. The external chart hook supplies
/// the actual ICP1 payload.
/// </summary>
internal static class CustomSongInstaller
{
    // The selector indexes this array directly by difficulty position:
    // Minimal=0, Evolved=1, Ultimate=2, Forbidden=3.  Shipped songs keep
    // all four positions even when a difficulty is not available.
    private const int kChartDifficultySlotCount = 4;

    private static readonly object Gate = new();
    private static readonly Dictionary<string, CustomSongConfig> LibrarySongsByChartKey =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, CustomSongConfig> LibrarySongsByBaseName =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, string> LibraryTemplateBaseNames =
        new(StringComparer.OrdinalIgnoreCase);
    private static readonly Dictionary<string, SongId> LibrarySongIdsByChartKey =
        new(StringComparer.OrdinalIgnoreCase);
    private static bool _complete;
    private static bool _installing;
    private static bool _earlyBoundaryUnavailableReported;
    private static bool _libraryInstalled;
    private static bool _collectionAssetsApplied;
    private static int _stockPackCount;
    private static int[] _collectionStylePackIds;
    private static int _collectionAssetProbeFrames;

    internal static CustomSongConfig ActiveConfig { get; private set; }
    internal static string CustomBaseName { get; private set; }
    internal static string TemplateBaseName { get; private set; }
    internal static string TemplateChartKey { get; private set; }
    internal static bool IsComplete => _complete;

    internal static void Tick()
    {
        if (!_libraryInstalled || _collectionAssetsApplied || ++_collectionAssetProbeFrames % 30 != 0)
            return;
        try
        {
            Il2CppReferenceArray<UnityEngine.Object> objects =
                Resources.FindObjectsOfTypeAll(Il2CppType.Of<PackSelectScene>());
            for (int index = 0; index < objects.Length; index++)
            {
                PackSelectScene scene = objects[index].TryCast<PackSelectScene>();
                if (scene?.packSelectSceneAssets is null) continue;
                ApplyCollectionAssets(scene.packSelectSceneAssets);
                return;
            }
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Warning("[CustomSong] collection visual-style probe failed: " + exception.Message);
        }
    }

    internal static CustomSongConfig FindActiveSongByChartKey(string chartKey)
    {
        if (chartKey is null) return null;
        if (LibrarySongsByChartKey.TryGetValue(chartKey, out CustomSongConfig song)) return song;
        CustomSongConfig config = ActiveConfig;
        return config is not null && !config.IsLibrary &&
               string.Equals(chartKey, config.ActiveChartKey, StringComparison.OrdinalIgnoreCase)
            ? config
            : null;
    }

    internal static CustomSongConfig FindActiveSongByBaseName(string baseName)
    {
        if (baseName is null) return null;
        if (LibrarySongsByBaseName.TryGetValue(baseName, out CustomSongConfig song)) return song;
        string normalized = Path.GetFileNameWithoutExtension(baseName);
        if (!string.Equals(normalized, baseName, StringComparison.OrdinalIgnoreCase) &&
            LibrarySongsByBaseName.TryGetValue(normalized, out song))
            return song;
        // Some game routes identify the selected difficulty rather than the
        // SongInfo base name. Accept its chart ID too, while retaining the
        // per-difficulty config required for the template audio lookup.
        if (LibrarySongsByChartKey.TryGetValue(normalized + ".spc", out song)) return song;
        CustomSongConfig config = ActiveConfig;
        return config is not null && !config.IsLibrary &&
               string.Equals(baseName, CustomBaseName, StringComparison.OrdinalIgnoreCase)
            ? config
            : null;
    }

    internal static string GetTemplateChartKey(CustomSongConfig song)
    {
        if (song is not null && LibraryTemplateKeys.TryGetValue(song.ChartKey, out string key)) return key;
        return TemplateChartKey;
    }

    internal static string GetTemplateBaseName(CustomSongConfig song)
    {
        if (song is not null && LibraryTemplateBaseNames.TryGetValue(song.ChartKey, out string baseName)) return baseName;
        return TemplateBaseName;
    }

    private static readonly Dictionary<string, string> LibraryTemplateKeys =
        new(StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Locates the live DataAccess asset without patching its IL2CPP
    /// deserialization callback. That callback is invoked for partially
    /// initialized objects and is unsafe to detour on this game version.
    /// </summary>
    internal static void ProbeForReadyData()
    {
        lock (Gate)
        {
            if (_complete) return;
            if (!CustomSongConfig.TryLoad(out CustomSongConfig config, out string configMessage))
            {
                _complete = true;
                CustomSongMod.Log.Msg("[CustomSong] not installed: " + configMessage);
                return;
            }

            // Carrier mode deliberately bypasses every SongData mutation. The
            // stock selected-song path will reach _S._Gab with this existing
            // key; CustomChartOverride replaces only that parser span.
            if (config.IsCarrierChartOverride)
            {
                ActiveConfig = config;
                _complete = true;
                CustomSongMod.Log.Msg(
                    $"[CustomSong] carrier-chart override armed: carrier={config.ActiveChartKey}; " +
                    $"external={config.ExternalChartStem}; parser={GetCarrierParserKey(config)}");
                return;
            }

            // Loose jackets are constructed only while copied jacket records
            // are built. No catalog preload can delay the early library-cache
            // boundary.

            Il2CppReferenceArray<UnityEngine.Object> objects;
            try
            {
                objects = Resources.FindObjectsOfTypeAll(Il2CppType.Of<DataAccess>());
            }
            catch (Exception exception)
            {
                CustomSongMod.Log.Warning("[CustomSong] DataAccess probe failed; retrying: " + exception.Message);
                return;
            }

            for (int index = 0; index < objects.Length; index++)
            {
                DataAccess dataAccess = objects[index].TryCast<DataAccess>();
                if (dataAccess is null || dataAccess.SongData is null || dataAccess.PackData is null)
                    continue;

                try
                {
                    if (config.IsLibrary)
                        InstallLibrary(dataAccess, config);
                    else
                        Install(dataAccess, config);
                    ActiveConfig = config;
                    _complete = true;
                    CustomSongMod.Log.Msg(config.IsLibrary
                        ? $"[CustomSong] installed custom library: collections={config.Collections.Length}; songs={LibrarySongsByChartKey.Count}"
                        : $"[CustomSong] installed songId={config.SongId} chart={config.ChartKey}");
                }
                catch (Exception exception)
                {
                    // A ready DataAccess with invalid metadata/configuration is
                    // terminal; do not repeatedly mutate or spam the log.
                    _complete = true;
                    CustomSongMod.Log.Error("[CustomSong] install failed: " + exception);
                }
                return;
            }
        }
    }

    /// <summary>
    /// Runs from the prefix of SongData._yOA. Unlike an OnUpdate probe, this
    /// is guaranteed to execute before the native method allocates _Ffb from
    /// allSongInfo.Length. DataAccess itself is constructed later on the
    /// shipped startup path, so this route resolves the sibling Scriptable
    /// Object assets directly and the later DataAccess consumes those same
    /// mutated instances.
    /// </summary>
    internal static void TryInstallBeforeSelectorCache(SongData targetSongData)
    {
        lock (Gate)
        {
            if (_complete || _installing || targetSongData is null) return;
            if (!CustomSongConfig.TryLoad(out CustomSongConfig config, out string message) || !config.IsLibrary)
                return;

            DataAccess dataAccess = FindDataAccessForSongData(targetSongData);
            PackData packData = dataAccess?.PackData ?? FindLoadedObject<PackData>();
            DynamicStringMapping dynamicStrings = dataAccess?.DynamicStringMapping ?? FindLoadedObject<DynamicStringMapping>();
            if (packData is null || dynamicStrings is null)
            {
                if (!_earlyBoundaryUnavailableReported)
                {
                    _earlyBoundaryUnavailableReported = true;
                    CustomSongMod.Log.Warning(
                        "[CustomSong] SongData cache boundary arrived before PackData/DynamicStringMapping were discoverable; library extension cannot safely continue.");
                }
                return;
            }

            _installing = true;
            try
            {
                InstallLibrary(targetSongData, packData, dynamicStrings, config);
                ActiveConfig = config;
                _complete = true;
                CustomSongMod.Log.Msg("[CustomSong] library installed at SongData._yOA cache boundary.");
            }
            catch (Exception exception)
            {
                _complete = true;
                CustomSongMod.Log.Error("[CustomSong] early library install failed: " + exception);
            }
            finally
            {
                _installing = false;
            }
        }
    }

    private static DataAccess FindDataAccessForSongData(SongData targetSongData)
    {
        Il2CppReferenceArray<UnityEngine.Object> objects =
            Resources.FindObjectsOfTypeAll(Il2CppType.Of<DataAccess>());
        for (int index = 0; index < objects.Length; index++)
        {
            DataAccess dataAccess = objects[index].TryCast<DataAccess>();
            if (dataAccess?.SongData is not null && dataAccess.PackData is not null &&
                dataAccess.DynamicStringMapping is not null &&
                dataAccess.SongData.Pointer == targetSongData.Pointer)
                return dataAccess;
        }
        return null;
    }

    private static T FindLoadedObject<T>() where T : Il2CppObjectBase
    {
        Il2CppReferenceArray<UnityEngine.Object> objects =
            Resources.FindObjectsOfTypeAll(Il2CppType.Of<T>());
        for (int index = 0; index < objects.Length; index++)
        {
            T value = objects[index].TryCast<T>();
            if (value is not null) return value;
        }
        return null;
    }

    private static string GetCarrierParserKey(CustomSongConfig config) =>
        string.IsNullOrWhiteSpace(config.ParserChartId)
            ? config.ActiveChartKey
            : config.ParserChartId + ".spc";

    /// <summary>
    /// Extends the real SongData table before its selector caches are built,
    /// then appends real PackInfo records.  This is deliberately an early,
    /// all-at-once operation: resizing after _Ffb has been created leaves the
    /// native selector's BitArray shorter than allSongInfo.
    /// </summary>
    private static void InstallLibrary(DataAccess dataAccess, CustomSongConfig root)
    {
        InstallLibrary(
            dataAccess.SongData ?? throw new InvalidOperationException("SongData is unavailable."),
            dataAccess.PackData ?? throw new InvalidOperationException("PackData is unavailable."),
            dataAccess.DynamicStringMapping ?? throw new InvalidOperationException("DynamicStringMapping is unavailable."),
            root);
    }

    private static void InstallLibrary(
        SongData songData,
        PackData packData,
        DynamicStringMapping dynamicStrings,
        CustomSongConfig root) {
        Il2CppReferenceArray<SongInfo> oldSongs = songData.allSongInfo ??
            throw new InvalidOperationException("SongData.allSongInfo is unavailable.");
        Il2CppReferenceArray<PackInfo> oldPacks = packData.PackInfo ??
            throw new InvalidOperationException("PackData.PackInfo is unavailable.");

        bool rebuildingEarlyCache = songData._Ffb is not null;
        if (rebuildingEarlyCache)
        {
            // This build deserializes SongData before its sibling assets, so
            // the pre-_yOA prefix cannot construct a coherent library. We are
            // still before any selector scene exists; invalidate every
            // length-coupled SongData cache and rebuild below after all arrays
            // have reached their final sizes.
            if (Resources.FindObjectsOfTypeAll(Il2CppType.Of<SongSelectScene>()).Length != 0)
                throw new InvalidOperationException(
                    "SongSelectScene already exists; refusing to replace its cached SongData filter.");
            songData._Ffb = null;
            songData._Efb = null;
            CustomSongMod.Log.Msg("[CustomSong] rebuilding startup SongData caches after early deserialization.");
        }

        List<LibrarySong> planned = new();
        HashSet<string> usedBaseNames = new(StringComparer.OrdinalIgnoreCase);
        HashSet<ushort> usedPackIds = new();
        HashSet<string> usedPackSlugs = new(StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < oldSongs.Length; index++)
        {
            SongInfo song = oldSongs[index];
            if (song is not null && !string.IsNullOrWhiteSpace(song.BaseName))
                usedBaseNames.Add(song.BaseName);
        }
        for (int index = 0; index < oldPacks.Length; index++)
        {
            PackInfo pack = oldPacks[index];
            if (pack is null) continue;
            usedPackIds.Add(pack.Id.Value);
            if (!string.IsNullOrWhiteSpace(pack.Slug)) usedPackSlugs.Add(pack.Slug);
        }

        int nextSongId = oldSongs.Length;
        int[] stylePackIds = new int[root.Collections.Length];
        for (int collectionIndex = 0; collectionIndex < root.Collections.Length; collectionIndex++) {
            CustomSongCollection collection = root.Collections[collectionIndex];
            int runtimePackId = oldPacks.Length + collectionIndex;
            if (runtimePackId > ushort.MaxValue)
                throw new InvalidOperationException("custom library exceeds the PackId range.");
            if (!usedPackIds.Add((ushort)runtimePackId) || !usedPackSlugs.Add(collection.Slug))
                throw new InvalidOperationException($"custom collection '{collection.Slug}' conflicts with an existing PackData id or slug.");

            if (collection.PackId != runtimePackId)
                CustomSongMod.Log.Msg($"[CustomSong] collection '{collection.Slug}' PackId {collection.PackId}->{runtimePackId} (PackInfo index).");
            collection.PackId = (ushort)runtimePackId;
            int stylePackIndex = FindPackIndexBySlug(
                oldPacks,
                string.IsNullOrWhiteSpace(collection.StylePackSlug) ? "act-4" : collection.StylePackSlug);
            stylePackIds[collectionIndex] = oldPacks[stylePackIndex].Id.Value;
            foreach (CustomSongConfig config in collection.Songs)
            {
                if (nextSongId > ushort.MaxValue)
                    throw new InvalidOperationException("custom library exceeds the SongId range.");
                if (config.TemplateSongId >= oldSongs.Length || oldSongs[config.TemplateSongId] is null)
                    throw new InvalidOperationException($"templateSongId {config.TemplateSongId} is not populated.");

                SongInfo templateSong = oldSongs[config.TemplateSongId];
                if (templateSong.ChartInfos is null || templateSong.ChartInfos.Length != kChartDifficultySlotCount)
                    throw new InvalidOperationException($"templateSongId {config.TemplateSongId} does not use the expected four-slot chart layout.");
                string baseName = GetCustomBaseName(config);
                if (string.IsNullOrWhiteSpace(baseName) || !usedBaseNames.Add(baseName))
                    throw new InvalidOperationException($"baseName '{baseName}' already belongs to a shipped or custom song.");

                List<LibraryChart> charts = new();
                foreach (CustomSongConfig entry in config.ExpandDifficulties())
                    charts.Add(new LibraryChart(entry, FindTemplateChart(templateSong, entry.Difficulty)));
                planned.Add(new LibrarySong(
                    collectionIndex,
                    new SongId { Value = (ushort)nextSongId++ },
                    config,
                    baseName,
                    templateSong,
                    charts));
            }
        }

        Il2CppReferenceArray<SongInfo> nextSongs = new(oldSongs.Length + planned.Count);
        for (int index = 0; index < oldSongs.Length; index++) nextSongs[index] = oldSongs[index];

        foreach (LibrarySong item in planned)
        {
            List<SongChartInfo> charts = new();
            foreach (LibraryChart source in item.Charts)
            {
                SongChartInfo chart = CloneChart(source.TemplateChart);
                chart.Id = source.Config.ChartId;
                chart.Available = true;
                chart.Difficulty = (ChartDifficultyFlag)source.Config.Difficulty;
                chart.Rating = source.Config.Rating;
                chart.LevelSectionIndicator = source.Config.LevelSectionIndicator ?? source.TemplateChart.LevelSectionIndicator;
                chart.DisplayChartDesigner = source.Config.ChartDesigner ?? source.TemplateChart.DisplayChartDesigner;
                chart.DisplayJacketDesigner = source.Config.JacketDesigner ?? source.TemplateChart.DisplayJacketDesigner;
                charts.Add(chart);
                CopyChartJacket(songData, source.TemplateChart.Id, source.Config.ChartId, source.Config);
                LibrarySongsByChartKey.Add(source.Config.ChartKey, source.Config);
                LibraryTemplateBaseNames.Add(source.Config.ChartKey, item.TemplateSong.BaseName);
                LibraryTemplateKeys.Add(source.Config.ChartKey, source.TemplateChart.Id + ".spc");
                LibrarySongIdsByChartKey.Add(source.Config.ChartKey, item.SongId);
            }

            SongInfo song = CloneSong(item.TemplateSong);
            song.Id = item.SongId;
            song.BaseName = item.BaseName;
            ApplySongMetadata(song, item.Config);
            song.ChartInfos = BuildChartSlots(charts);
            nextSongs[item.SongId.Value] = song;

            CopySongJacket(songData, item.TemplateSong.Id, item.SongId, item.Config);
            AppendSongDisplayMetadata(dynamicStrings, item.SongId, item.Config);
            // Audio is requested from the song-level BaseName and therefore
            // resolves through this root config, not one of its positional
            // difficulty configs.  Keep the corresponding template base
            // name under the root ChartKey as well; otherwise
            // CustomSongAudioOverride has a valid config but no template
            // route and the native <customBaseName>.wav lookup stalls before
            // it can create a controller.
            LibraryTemplateBaseNames[item.Config.ChartKey] = item.TemplateSong.BaseName;
            LibrarySongsByBaseName.Add(item.BaseName, item.Config);
        }

        songData.allSongInfo = nextSongs;
        AppendCollections(packData, root.Collections, planned);
        AppendCollectionDisplayMetadata(dynamicStrings, root.Collections);
        // Rebuild all serialized string-map dictionaries as the game itself
        // does after asset deserialization. Calling only _TZA on the three
        // touched maps leaves their lookup state stale on this build.
        dynamicStrings.OnAfterDeserialize();

        // OnAfterDeserialize clears resolver dictionaries. Build them only
        // after every array has its final size, including the tutorial filter.
        songData.OnAfterDeserialize();
        NativeSongDataBridge.ResetSongIdJacketCache(songData);
        songData._yOA();
        packData.OnAfterDeserialize();

        _stockPackCount = oldPacks.Length;
        _collectionStylePackIds = stylePackIds;
        _libraryInstalled = true;
        CustomSongMod.Log.Msg(
            $"[CustomSong] expanded SongData {oldSongs.Length}->{nextSongs.Length}; PackData {oldPacks.Length}->{packData.PackInfo.Length}; selectorFilter={songData._Ffb?.Length ?? -1}; rebuiltEarlyCache={rebuildingEarlyCache}");
    }

    private static void AppendCollections(
        PackData packData,
        CustomSongCollection[] collections,
        List<LibrarySong> planned)
    {
        Il2CppReferenceArray<PackInfo> oldPacks = packData.PackInfo;
        Il2CppReferenceArray<PackInfo> nextPacks = new(oldPacks.Length + collections.Length);
        for (int index = 0; index < oldPacks.Length; index++) nextPacks[index] = oldPacks[index];
        for (int collectionIndex = 0; collectionIndex < collections.Length; collectionIndex++)
        {
            List<SongId> ids = new();
            foreach (LibrarySong item in planned)
                if (item.CollectionIndex == collectionIndex) ids.Add(item.SongId);
            Il2CppStructArray<SongId> songIds = new(ids.Count);
            for (int songIndex = 0; songIndex < ids.Count; songIndex++) songIds[songIndex] = ids[songIndex];
            CustomSongCollection collection = collections[collectionIndex];
            nextPacks[oldPacks.Length + collectionIndex] = new PackInfo
            {
                Id = new PackId { Value = collection.PackId },
                Slug = collection.Slug,
                SongIds = songIds,
            };
        }
        packData.PackInfo = nextPacks;
    }

    private static int FindPackIndexBySlug(Il2CppReferenceArray<PackInfo> packs, string slug)
    {
        for (int index = 0; index < packs.Length; index++)
            if (string.Equals(packs[index]?.Slug, slug, StringComparison.OrdinalIgnoreCase)) return index;
        throw new InvalidOperationException($"stylePackSlug '{slug}' was not found in PackData.");
    }

    private static void AppendSongDisplayMetadata(
        DynamicStringMapping dynamicStrings,
        SongId songId,
        CustomSongConfig config)
    {
        AppendText(dynamicStrings?.songIdTitleTypeMapping, songId,
            string.IsNullOrWhiteSpace(config.SongTitle) ? "Custom Song" : config.SongTitle, "title");
        AppendText(dynamicStrings?.songIdArtistTypeMapping, songId,
            string.IsNullOrWhiteSpace(config.Artist) ? "Custom Artist" : config.Artist, "artist");
        AppendText(dynamicStrings?.jacketIllustratorNameTypeMapping, songId,
            string.IsNullOrWhiteSpace(config.JacketDesigner) ? "Custom jacket" : config.JacketDesigner, "jacket illustrator");
    }

    private static void AppendCollectionDisplayMetadata(
        DynamicStringMapping dynamicStrings,
        CustomSongCollection[] collections)
    {
        if (dynamicStrings?.packIdTypeMapping is null)
            throw new InvalidOperationException("Pack title mapping is unavailable.");
        foreach (CustomSongCollection collection in collections)
            AppendText(dynamicStrings.packIdTypeMapping, new PackId { Value = collection.PackId }, collection.Title, "pack title");
    }

    private static void AppendText<T>(
        DynamicStringMapping.StringTypeMapping<T> mapping,
        T id,
        string text,
        string fieldName)
    {
        if (mapping is null || mapping.Ids is null || mapping.IdValues is null)
            throw new InvalidOperationException($"{fieldName} mapping is unavailable.");
        DynamicStringMapping.TextMappingValues values = new()
        {
            English = text,
            Japanese = text,
            Korean = text,
            TraditionalChinese = text,
            SimplifiedChinese = text,
        };
        int existingIndex = -1;
        for (int index = 0; index < mapping.Ids.Count; index++)
        {
            if (EqualityComparer<T>.Default.Equals(mapping.Ids[index], id))
            {
                existingIndex = index;
                break;
            }
        }
        if (existingIndex >= 0)
        {
            if (existingIndex >= mapping.IdValues.Count)
                throw new InvalidOperationException($"{fieldName} mapping has mismatched id/value counts.");
            mapping.IdValues[existingIndex] = values;
            if (mapping.IdStr is not null && existingIndex < mapping.IdStr.Count)
                mapping.IdStr[existingIndex] = text;
        }
        else
        {
            mapping.Ids.Add(id);
            mapping.IdValues.Add(values);
            mapping.IdStr?.Add(text);
        }
        mapping._TZA();
    }

    /// <summary>
    /// DynamicStringMapping can deserialize after the early SongData install.
    /// Its serialized lists survive that event but its runtime dictionaries are
    /// recreated, so restore all custom records without ever duplicating keys.
    /// </summary>
    internal static void ReapplyLibraryDisplayMetadata(DynamicStringMapping dynamicStrings)
    {
        CustomSongConfig root = ActiveConfig;
        if (!_libraryInstalled || root?.Collections is not { Length: > 0 } || dynamicStrings is null)
            return;

        foreach (CustomSongCollection collection in root.Collections)
        {
            foreach (CustomSongConfig config in collection.Songs)
            {
                foreach (CustomSongConfig entry in config.ExpandDifficulties())
                {
                    if (!LibrarySongIdsByChartKey.TryGetValue(entry.ChartKey, out SongId songId)) continue;
                    AppendSongDisplayMetadata(dynamicStrings, songId, config);
                    break;
                }
            }
            AppendText(dynamicStrings.packIdTypeMapping, new PackId { Value = collection.PackId },
                collection.Title, "pack title");
        }

        CustomSongMod.Log.Msg("[CustomSong] reapplied custom title/artist mappings after DynamicStringMapping deserialization.");
    }

    internal static void ApplyCollectionAssets(PackSelectSceneAssets assets) {
        if (!_libraryInstalled || _collectionAssetsApplied || assets?.packToAssets is null) return;
        try
        {
            var oldAssets = assets.packToAssets;
            if (oldAssets.Length < _stockPackCount)
                throw new InvalidOperationException($"pack style table shrank to {oldAssets.Length}; expected at least {_stockPackCount}.");
            assets.packToAssets = AssignCollectionStyles(oldAssets, _collectionStylePackIds, _stockPackCount);
            _collectionAssetsApplied = true;
            CustomSongMod.Log.Msg($"[CustomSong] assigned collection visual styles by PackId: {oldAssets.Length}->{assets.packToAssets.Length}; customStart={_stockPackCount}; count={_collectionStylePackIds.Length}");
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Error("[CustomSong] collection visual-style install failed: " + exception);
        }
    }

    private static Il2CppReferenceArray<T> AssignCollectionStyles<T>(
        Il2CppReferenceArray<T> source, int[] sourcePackIds, int firstCustomPackId)
        where T : Il2CppObjectBase {
        Il2CppReferenceArray<T> next = new(Math.Max(source.Length, firstCustomPackId + sourcePackIds.Length));

        for (int index = 0; index < source.Length; index++) next[index] = source[index];
        for (int index = 0; index < sourcePackIds.Length; index++) {
            int sourcePackId = sourcePackIds[index];
            if (sourcePackId < 0 || sourcePackId >= source.Length)
                throw new IndexOutOfRangeException($"style PackId {sourcePackId} is outside {source.Length} visual assets.");
            next[firstCustomPackId + index] = source[sourcePackId];
        }

        return next;
    }

    private sealed class LibraryChart
    {
        internal LibraryChart(CustomSongConfig config, SongChartInfo templateChart)
        {
            Config = config;
            TemplateChart = templateChart;
        }
        internal CustomSongConfig Config { get; }
        internal SongChartInfo TemplateChart { get; }
    }

    private sealed class LibrarySong
    {
        internal LibrarySong(int collectionIndex, SongId songId, CustomSongConfig config, string baseName, SongInfo templateSong, List<LibraryChart> charts)
        {
            CollectionIndex = collectionIndex;
            SongId = songId;
            Config = config;
            BaseName = baseName;
            TemplateSong = templateSong;
            Charts = charts;
        }
        internal int CollectionIndex { get; }
        internal SongId SongId { get; }
        internal CustomSongConfig Config { get; }
        internal string BaseName { get; }
        internal SongInfo TemplateSong { get; }
        internal List<LibraryChart> Charts { get; }
    }

    private static void Install(DataAccess dataAccess, CustomSongConfig config)
    {
        SongData songData = dataAccess.SongData ?? throw new InvalidOperationException("SongData is unavailable.");
        PackData packData = dataAccess.PackData ?? throw new InvalidOperationException("PackData is unavailable.");
        Il2CppReferenceArray<SongInfo> songs = songData.allSongInfo ?? throw new InvalidOperationException("SongData.allSongInfo is unavailable.");

        // SongId is an index into this fixed-size table.  The release database
        // intentionally contains invalid (Id == 0) slots, and the selector has
        // fixed-range consumers for this original table.  Do not resize it.
        if (config.SongId == 0 || config.SongId >= songs.Length)
            throw new InvalidOperationException($"songId {config.SongId} is outside the preallocated SongData range.");
        SongInfo slot = songs[config.SongId];
        if (slot.Id.Value != 0 || !string.IsNullOrWhiteSpace(slot.BaseName) ||
            (slot.ChartInfos is not null && slot.ChartInfos.Length != 0))
            throw new InvalidOperationException($"songId {config.SongId} is not an unused SongData slot.");
        if (config.TemplateSongId >= songs.Length || songs[config.TemplateSongId] is null)
            throw new InvalidOperationException($"templateSongId {config.TemplateSongId} is not populated.");
        if (config.TemplateSongId == config.SongId)
            throw new InvalidOperationException("templateSongId cannot be the custom slot.");
        SongInfo templateSong = songs[config.TemplateSongId];
        SongChartInfo templateChart = FindTemplateChart(templateSong, config.Difficulty);
        if (templateSong.ChartInfos is null || templateSong.ChartInfos.Length != kChartDifficultySlotCount)
            throw new InvalidOperationException("The template song does not use the expected four-slot chart layout.");

        // Initialize the original caches before mutation. The later custom
        // record receives a unique BaseName and explicitly rebuilds the name
        // map; the fixed-size selector BitArray must remain allocated.
        songData._yOA();

        string customBaseName = GetCustomBaseName(config);
        if (string.IsNullOrWhiteSpace(customBaseName))
            throw new InvalidOperationException("baseName/chartId must provide a non-empty custom song identity.");
        if (FindExistingBaseName(songs, customBaseName))
            throw new InvalidOperationException($"baseName '{customBaseName}' already belongs to a song.");
        int packIndex = FindPack(packData, config, templateSong.Id);

        SongId customSongId = new() { Value = config.SongId };
        SongChartInfo customChart = CloneChart(templateChart);
        // Keep the injected chart's identity unique. The streaming cache hook
        // translates just this logical key to the template's mounted payload
        // source; reusing the template ID here caused duplicate chart identity
        // resolution before the parser could be reached.
        customChart.Id = config.ChartId;
        customChart.Available = true;
        customChart.Difficulty = (ChartDifficultyFlag)config.Difficulty;
        customChart.Rating = config.Rating;
        customChart.LevelSectionIndicator = config.LevelSectionIndicator ?? templateChart.LevelSectionIndicator;
        customChart.DisplayChartDesigner = config.ChartDesigner ?? templateChart.DisplayChartDesigner;
        customChart.DisplayJacketDesigner = config.JacketDesigner ?? templateChart.DisplayJacketDesigner;

        SongInfo customSong = CloneSong(templateSong);
        customSong.Id = customSongId;
        // SongData's filtered selector span deduplicates by the cached
        // BaseName -> SongId map. A shared template BaseName therefore makes
        // the second record unreachable even though its table slot and pack
        // membership are valid. Give the custom record its own identity and
        // supply <customBaseName>.wav as a streaming alias below.
        customSong.BaseName = customBaseName;
        ApplySongMetadata(customSong, config);
        customSong.ChartInfos = BuildChartSlots(config, customChart);

        songs[config.SongId] = customSong;

        // Rebuild the BaseName identity cache from the now-unique record. It
        // was initialized before mutation specifically to avoid the earlier
        // duplicate-key exception; nulling it is now safe and necessary.
        songData._Efb = null;

        CopySongJacket(songData, templateSong.Id, customSongId, config);
        // SongSelect's card uses the song-ID jacket entry, but gameplay's
        // transition resolves a second jacket entry by ChartInfo.Id. Without
        // this separate mapping a unique custom chart falls back to "No
        // jacket" before the chart-source path is entered.
        CopyChartJacket(songData, templateChart.Id, config.ChartId, config);
        PopulateSongDisplayMetadata(dataAccess.DynamicStringMapping, customSongId, config);
        AddPackMembership(packData, packIndex, customSongId);

        // The appended jacket records change their serialized-array counts, so
        // _yOA rebuilds those two dictionaries.  Its already-populated BaseName
        // dictionary and fixed-size tutorial filter intentionally stay intact.
        songData.OnAfterDeserialize();

        // Retain the original fixed-size selector filter. The verified slot is
        // already false in it; replacing or resizing this array is unsafe.
        if (songData._Ffb is null || config.SongId >= songData._Ffb.Length)
            throw new InvalidOperationException("SongData selector filter is unavailable or too short.");
        bool filterBefore = songData._Ffb.Get(config.SongId);

        packData.OnAfterDeserialize();
        TemplateBaseName = templateSong.BaseName;
        CustomBaseName = customBaseName;
        TemplateChartKey = templateChart.Id + ".spc";
        CustomSongMod.Log.Msg($"[CustomSong] selector filter slot={config.SongId}: {filterBefore}; baseName={customBaseName}; chart={config.ChartKey}; source={TemplateChartKey}");
    }

    private static SongChartInfo FindTemplateChart(SongInfo song, byte difficulty)
    {
        Il2CppReferenceArray<SongChartInfo> charts = song.ChartInfos ?? throw new InvalidOperationException("Template has no charts.");
        for (int index = 0; index < charts.Length; index++)
            if (charts[index].Difficulty == (ChartDifficultyFlag)difficulty)
                return charts[index];
        throw new InvalidOperationException($"Template has no difficulty {difficulty} chart.");
    }

    private static Il2CppReferenceArray<SongChartInfo> BuildChartSlots(CustomSongConfig config, SongChartInfo customChart) =>
        BuildChartSlots(new[] { customChart });

    private static Il2CppReferenceArray<SongChartInfo> BuildChartSlots(IEnumerable<SongChartInfo> customCharts)
    {
        // SongSelect's card constructor performs ChartInfos[currentIndex]
        // before it evaluates availability. A single custom chart therefore
        // needs three structural placeholders, like the game's tutorial
        // records, rather than a one-element array.
        Il2CppReferenceArray<SongChartInfo> slots = new(kChartDifficultySlotCount);
        for (int index = 0; index < slots.Length; index++)
            slots[index] = new SongChartInfo
            {
                Id = string.Empty,
                Available = false,
                Difficulty = 0,
                DisplayChartDesigner = string.Empty,
                DisplayJacketDesigner = string.Empty,
                Rating = 0,
                LevelSectionIndicator = "?",
            };

        foreach (SongChartInfo customChart in customCharts)
            slots[DifficultyToSlot((byte)customChart.Difficulty)] = customChart;
        return slots;
    }

    private static int DifficultyToSlot(byte difficulty) => difficulty switch
    {
        1 => 0,
        2 => 1,
        4 => 2,
        8 => 3,
        _ => throw new InvalidOperationException($"Unsupported difficulty {difficulty}."),
    };

    /// <summary>
    /// GameScene derives the logical chart key as BaseName plus the positional
    /// difficulty slot (for example, alamode + 0 = alamode0.spc). When no
    /// explicit song base name is supplied, preserve that relationship for a
    /// conventional chart ID such as custom0. This identity is still unique
    /// in SongData; it is not the external file name.
    /// </summary>
    internal static string GetCustomBaseName(CustomSongConfig config)
    {
        if (!string.IsNullOrWhiteSpace(config.BaseName)) return config.BaseName;

        string chartId = config.ChartId;
        int slot = DifficultyToSlot(config.Difficulty);
        if (chartId.Length > 1 && chartId[^1] == (char)('0' + slot))
            return chartId[..^1];
        return chartId;
    }

    // IL2CPP value-type wrappers can point at a boxed/array-backed value. Do
    // not assign a template wrapper and then set its properties: that risks
    // changing the original database record instead of the appended one.
    private static SongInfo CloneSong(SongInfo source) => new() {
        Id = source.Id,
        BaseName = source.BaseName,
        CharacterIdentifier = source.CharacterIdentifier,
        ChartInfos = source.ChartInfos,
        PreviewStartSeconds = source.PreviewStartSeconds,
        PreviewEndSeconds = source.PreviewEndSeconds,
        LocalizationToTitleReadingOverride = source.LocalizationToTitleReadingOverride,
        ArtistReadingOverride = source.ArtistReadingOverride,
        Copyright = source.Copyright,
        GameplayBackground = source.GameplayBackground,
        RewardStyle = source.RewardStyle,
    };

    private static void ApplySongMetadata(SongInfo song, CustomSongConfig config)
    {
        if (config.PreviewStartSeconds.HasValue)
            song.PreviewStartSeconds = config.PreviewStartSeconds.Value;
        if (config.PreviewEndSeconds.HasValue)
            song.PreviewEndSeconds = config.PreviewEndSeconds.Value;
        SetOptionalEnumProperty(song, nameof(SongInfo.CharacterIdentifier), config.CharacterIdentifier);
        SetOptionalEnumProperty(song, nameof(SongInfo.GameplayBackground), config.GameplayBackground);
    }

    private static void SetOptionalEnumProperty(object target, string propertyName, string configuredValue)
    {
        if (string.IsNullOrWhiteSpace(configuredValue)) return;
        PropertyInfo property = target.GetType().GetProperty(propertyName);
        if (property is null || !property.PropertyType.IsEnum)
            throw new InvalidOperationException($"{propertyName} is not an enum property on {target.GetType().Name}.");
        try
        {
            property.SetValue(target, Enum.Parse(property.PropertyType, configuredValue, ignoreCase: true));
        }
        catch (ArgumentException)
        {
            throw new InvalidOperationException($"Unknown {propertyName} value '{configuredValue}'. Use a member name from the game's SongInfo data.");
        }
    }

    private static SongChartInfo CloneChart(SongChartInfo source) => new()
    {
        Id = source.Id,
        Available = source.Available,
        Difficulty = source.Difficulty,
        DisplayChartDesigner = source.DisplayChartDesigner,
        DisplayJacketDesigner = source.DisplayJacketDesigner,
        Rating = source.Rating,
        LevelSectionIndicator = source.LevelSectionIndicator,
    };

    private static int FindPack(PackData packData, CustomSongConfig config, SongId templateSongId)
    {
        Il2CppReferenceArray<PackInfo> packs = packData.PackInfo ?? throw new InvalidOperationException("PackData.PackInfo is unavailable.");
        for (int index = 0; index < packs.Length; index++)
        {
            PackInfo pack = packs[index];
            if (!string.IsNullOrWhiteSpace(config.PackSlug) &&
                !string.Equals(pack.Slug, config.PackSlug, StringComparison.OrdinalIgnoreCase))
                continue;
            if (string.IsNullOrWhiteSpace(config.PackSlug) && !pack._VnA(templateSongId))
                continue;
            return index;
        }
        throw new InvalidOperationException(config.PackSlug is null
            ? "No pack contains the template song."
            : $"Pack slug '{config.PackSlug}' was not found.");
    }

    private static bool FindExistingBaseName(Il2CppReferenceArray<SongInfo> songs, string baseName)
    {
        for (int songIndex = 0; songIndex < songs.Length; songIndex++)
        {
            SongInfo song = songs[songIndex];
            if (song is not null && string.Equals(song.BaseName, baseName, StringComparison.OrdinalIgnoreCase))
                return true;
        }
        return false;
    }

    private static void CopySongJacket(SongData songData, SongId templateSongId, SongId customSongId, CustomSongConfig config)
    {
        Il2CppReferenceArray<SongJacketEntry> oldEntries = songData.songIdJacketMaterials;
        SongJacketEntry template = null;
        int customEntryIndex = -1;
        for (int index = 0; index < oldEntries.Length; index++)
        {
            SongJacketEntry entry = oldEntries[index];
            if (entry.SongId.Value == templateSongId.Value) template = entry;
            if (entry.SongId.Value == customSongId.Value) customEntryIndex = index;
        }
        if (template is null) throw new InvalidOperationException("Template song jacket entry was not found.");

        SongJacketEntry copy = new()
        {
            SongId = customSongId,
            JacketLargeMaterial = template.JacketLargeMaterial,
            JacketSmallMaterial = template.JacketSmallMaterial,
        };
        bool applied = CustomJacketOverride.ApplySelectorReferences(copy, config);

        // The release has a reserved selector-jacket record for the otherwise
        // absent SongId 83. Appending a second record leaves the native map
        // with its first (template) value, so replace that record in place.
        if (customEntryIndex >= 0)
        {
            oldEntries[customEntryIndex] = copy;
            CustomSongMod.Log.Msg("[CustomSong] replaced reserved selector jacket entry for songId=" + customSongId.Value);
        }
        else
        {
            songData.songIdJacketMaterials = Append(oldEntries, copy);
        }
        if (!applied) CustomJacketOverride.QueueSelector(songData, customSongId, config);
    }

    private static void AddPackMembership(PackData packData, int packIndex, SongId customSongId)
    {
        Il2CppReferenceArray<PackInfo> oldPacks = packData.PackInfo;
        Il2CppReferenceArray<PackInfo> nextPacks = new(oldPacks.Length);
        for (int index = 0; index < oldPacks.Length; index++) nextPacks[index] = oldPacks[index];

        PackInfo sourcePack = oldPacks[packIndex];
        Il2CppStructArray<SongId> oldIds = sourcePack.SongIds;
        Il2CppStructArray<SongId> nextIds = new(oldIds.Length + 1);
        for (int index = 0; index < oldIds.Length; index++) nextIds[index] = oldIds[index];
        nextIds[oldIds.Length] = customSongId;
        PackInfo pack = new()
        {
            Id = sourcePack.Id,
            Slug = sourcePack.Slug,
            SongIds = nextIds,
        };
        nextPacks[packIndex] = pack;
        packData.PackInfo = nextPacks;
    }

    private static void CopyChartJacket(SongData songData, string templateChartId, string customChartId, CustomSongConfig config)
    {
        Il2CppReferenceArray<SongDataChartJacketEntry> oldEntries = songData.chartIdJacketMaterials;
        for (int index = 0; index < oldEntries.Length; index++)
        {
            SongDataChartJacketEntry source = oldEntries[index];
            if (!string.Equals(source.ChartId, templateChartId, StringComparison.OrdinalIgnoreCase)) continue;
            SongDataChartJacketEntry copy = new()
            {
                ChartId = customChartId,
                JacketLargeMaterial = source.JacketLargeMaterial,
                JacketSmallMaterial = source.JacketSmallMaterial,
            };
            bool applied = CustomJacketOverride.ApplyChartReferences(copy, config);
            songData.chartIdJacketMaterials = Append(oldEntries, copy);
            if (!applied) CustomJacketOverride.QueueChart(songData, customChartId, config);
            return;
        }
        throw new InvalidOperationException($"Template chart jacket entry '{templateChartId}' was not found.");
    }

    private static void PopulateSongDisplayMetadata(
        DynamicStringMapping dynamicStrings,
        SongId customSongId,
        CustomSongConfig config)
    {
        if (dynamicStrings is null)
            throw new InvalidOperationException("DynamicStringMapping is unavailable.");

        // Slot 66 already has serialized placeholder entries. Replace those
        // values in memory rather than adding duplicate SongId keys. The live
        // Str.Strings provider delegates to this same mapping object.
        ReplaceSongText(dynamicStrings.songIdTitleTypeMapping, customSongId, config.SongTitle, "title");
        ReplaceSongText(dynamicStrings.songIdArtistTypeMapping, customSongId, config.Artist, "artist");
        ReplaceSongText(dynamicStrings.jacketIllustratorNameTypeMapping, customSongId, config.JacketDesigner, "jacket illustrator");
    }

    private static void ReplaceSongText(
        DynamicStringMapping.StringTypeMapping<SongId> mapping,
        SongId songId,
        string text,
        string fieldName)
    {
        if (mapping is null || mapping.Ids is null || mapping.IdValues is null)
            throw new InvalidOperationException($"Song {fieldName} mapping is unavailable.");

        int index = -1;
        for (int item = 0; item < mapping.Ids.Count; item++)
        {
            if (mapping.Ids[item].Value == songId.Value)
            {
                index = item;
                break;
            }
        }
        if (index < 0 || index >= mapping.IdValues.Count)
            throw new InvalidOperationException($"SongId {songId.Value} has no reserved {fieldName} mapping slot.");

        string resolved = string.IsNullOrWhiteSpace(text) ? "Custom Song" : text;
        DynamicStringMapping.TextMappingValues values = new()
        {
            English = resolved,
            Japanese = resolved,
            Korean = resolved,
            TraditionalChinese = resolved,
            SimplifiedChinese = resolved,
        };
        mapping.IdValues[index] = values;
        if (mapping.IdStr is not null && index < mapping.IdStr.Count)
            mapping.IdStr[index] = resolved;

        // Recreate the runtime SongId -> TextMappingValues dictionary from the
        // parallel serialized lists after changing the reserved entry.
        mapping._TZA();
    }

    private static Il2CppReferenceArray<T> Append<T>(Il2CppReferenceArray<T> source, T value) where T : Il2CppSystem.ValueType
    {
        Il2CppReferenceArray<T> next = new(source.Length + 1);
        for (int index = 0; index < source.Length; index++) next[index] = source[index];
        next[source.Length] = value;
        return next;
    }
}
