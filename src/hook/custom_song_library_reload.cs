using System;
using System.Collections.Generic;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
#if BEPINEX
using ifapp.Game;
#else
using Il2Cppifapp.Game;
#endif
#if BEPINEX
using ifapp.Game.Common;
#else
using Il2Cppifapp.Game.Common;
#endif
#if BEPINEX
using ifapp.Game.Data;
#else
using Il2Cppifapp.Game.Data;
#endif

namespace InFalsusCustomSongHook;

internal static partial class CustomSongInstaller {
    private static CustomLibrarySnapshot _baseline;
    private static bool _reloading;
    private static readonly Dictionary<IntPtr, Action> CollectionAssets = new();

    internal static void ReloadLibrary(CustomSongConfig config, Func<bool> contentsCurrent) {
        lock (Gate) {
            if (_installing) throw new InvalidOperationException("The song library is still being installed.");
            SongData songs = _baseline?.Songs ?? FindLoadedObject<SongData>();
            PackData packs = _baseline?.Packs ?? FindLoadedObject<PackData>();
            DynamicStringMapping strings = _baseline?.Strings ?? FindLoadedObject<DynamicStringMapping>();
            if (songs is null || packs is null || strings is null)
                throw new InvalidOperationException("The game's song library is unavailable.");
            var payloads = CustomChartOverride.PrepareLibrary(config);
            if (!contentsCurrent()) throw new InvalidOperationException("The chart library changed during payload preparation.");
            CustomSongAudioOverride.Dispose();
            _baseline ??= new CustomLibrarySnapshot(songs, packs, strings);
            CustomLibrarySnapshot previous = new(songs, packs, strings);
            LibraryState state = new();
            using var jackets = CustomJacketOverride.BeginReload();
            _installing = true;
            _reloading = true;
            try {
                ClearLibraryLookups();
                _baseline.Restore();
                InstallLibrary(songs, packs, strings, config);
                if (!contentsCurrent()) throw new InvalidOperationException("The chart library changed during installation.");
                ActiveConfig = config.Collections.Length == 0 ? null : config;
                _complete = true;
                CustomChartOverride.CommitLibrary(payloads);
                jackets.Commit();
            } catch {
                ClearLibraryLookups();
                previous.Restore();
                previous.Rebuild();
                state.Restore();
                throw;
            } finally {
                _installing = false;
                _reloading = false;
            }
            ResetCollectionAssets();
            CustomPackJacketOverride.Dispose();
        }
    }

    internal static void RestoreLibrary() {
        lock (Gate) {
            _installing = true;
            try {
                ClearLibraryLookups();
                if (_baseline is not null) {
                    _baseline.Restore();
                    _baseline.Rebuild();
                }
                ResetCollectionAssets();
                CollectionAssets.Clear();
                _baseline = null;
                _complete = false;
            } finally {
                _installing = false;
            }
        }
    }

    private static void ClearLibraryLookups() {
        LibrarySongsByChartKey.Clear();
        LibrarySongsByBaseName.Clear();
        LibrarySongIdsByChartKey.Clear();
        LibraryTemplateBaseNames.Clear();
        LibraryTemplateKeys.Clear();
        ActiveConfig = null;
        _libraryInstalled = false;
        _collectionAssetProbeComplete = false;
        _collectionAssetProbeFrames = 0;
    }

    private static void CaptureCollectionAssets(PackSelectSceneAssets assets) {
        if (CollectionAssets.ContainsKey(assets.Pointer)) return;
        var styles = assets.packToAssets;
        List<Action> restorations = new();
        for (int index = 0; index < styles.Length; index++) {
            var style = styles[index];
            if (style is null) continue;
            int position = index;
            var large = new[] { style.PackSelectLargeNonCompleted, style.PackSelectLargeNonCompletedHover,
                style.PackSelectLargeCompleted, style.PackSelectLargeCompletedHover };
            var small = new[] { style.PackSelectNonCompleted, style.PackSelectNonCompletedHover,
                style.PackSelectCompleted, style.PackSelectCompletedHover };
            var song = new[] { style.SongSelectLargeNonCompleted, style.SongSelectLargeNonCompletedHover,
                style.SongSelectLargeCompleted, style.SongSelectLargeCompletedHover };
            restorations.Add(() => {
                style.PackSelectLargeNonCompleted = large[0];
                style.PackSelectLargeNonCompletedHover = large[1];
                style.PackSelectLargeCompleted = large[2];
                style.PackSelectLargeCompletedHover = large[3];
                style.PackSelectNonCompleted = small[0];
                style.PackSelectNonCompletedHover = small[1];
                style.PackSelectCompleted = small[2];
                style.PackSelectCompletedHover = small[3];
                style.SongSelectLargeNonCompleted = song[0];
                style.SongSelectLargeNonCompletedHover = song[1];
                style.SongSelectLargeCompleted = song[2];
                style.SongSelectLargeCompletedHover = song[3];
                styles[position] = style;
            });
        }
        CollectionAssets.Add(assets.Pointer, () => {
            if (assets == null) return;
            foreach (Action restore in restorations) restore();
            assets.packToAssets = styles;
        });
    }

    private static void ResetCollectionAssets() {
        foreach (Action restore in CollectionAssets.Values) restore();
        _collectionAssetProbeComplete = false;
    }

    private sealed class LibraryState {
        private readonly CustomSongConfig m_config = ActiveConfig;
        private readonly bool m_installed = _libraryInstalled;
        private readonly bool m_complete = _complete;
        private readonly int m_packCount = _stockPackCount;
        private readonly int[] m_styles = _collectionStylePackIds;
        private readonly Dictionary<string, CustomSongConfig> m_charts = new(LibrarySongsByChartKey, StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, CustomSongConfig> m_songs = new(LibrarySongsByBaseName, StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, SongId> m_ids = new(LibrarySongIdsByChartKey, StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, string> m_templates = new(LibraryTemplateBaseNames, StringComparer.OrdinalIgnoreCase);
        private readonly Dictionary<string, string> m_keys = new(LibraryTemplateKeys, StringComparer.OrdinalIgnoreCase);

        internal void Restore() {
            Copy(m_charts, LibrarySongsByChartKey);
            Copy(m_songs, LibrarySongsByBaseName);
            Copy(m_ids, LibrarySongIdsByChartKey);
            Copy(m_templates, LibraryTemplateBaseNames);
            Copy(m_keys, LibraryTemplateKeys);
            ActiveConfig = m_config;
            _libraryInstalled = m_installed;
            _complete = m_complete;
            _stockPackCount = m_packCount;
            _collectionStylePackIds = m_styles;
        }

        private static void Copy<T>(Dictionary<string, T> source, Dictionary<string, T> target) {
            target.Clear();
            foreach (var entry in source) target.Add(entry.Key, entry.Value);
        }
    }
}
