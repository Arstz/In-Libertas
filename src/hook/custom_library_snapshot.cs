using System;
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

internal sealed class CustomLibrarySnapshot {
    internal SongData Songs { get; }
    internal PackData Packs { get; }
    internal DynamicStringMapping Strings { get; }
    private readonly Il2CppReferenceArray<SongInfo> m_songs;
    private readonly Il2CppReferenceArray<PackInfo> m_packs;
    private readonly Il2CppReferenceArray<SongJacketEntry> m_songJackets;
    private readonly Il2CppReferenceArray<SongDataChartJacketEntry> m_chartJackets;
    private readonly Action[] m_restoreText;

    internal CustomLibrarySnapshot(SongData songs, PackData packs, DynamicStringMapping strings) {
        Songs = songs;
        Packs = packs;
        Strings = strings;
        m_songs = songs.allSongInfo;
        m_packs = packs.PackInfo;
        m_songJackets = songs.songIdJacketMaterials;
        m_chartJackets = songs.chartIdJacketMaterials;
        m_restoreText = new[] {
            CaptureText(strings.songIdTitleTypeMapping), CaptureText(strings.songIdArtistTypeMapping),
            CaptureText(strings.jacketIllustratorNameTypeMapping), CaptureText(strings.packIdTypeMapping),
        };
    }

    internal void Restore() {
        Songs.allSongInfo = m_songs;
        Packs.PackInfo = m_packs;
        Songs.songIdJacketMaterials = m_songJackets;
        Songs.chartIdJacketMaterials = m_chartJackets;
        foreach (Action restore in m_restoreText) restore();
        Songs._Efb = null;
        Songs._Ffb = null;
        NativeSongDataBridge.ResetSongIdJacketCache(Songs);
        NativeSongDataBridge.ResetChartIdJacketCache(Songs);
    }

    internal void Rebuild() {
        Strings.OnAfterDeserialize();
        Songs.OnAfterDeserialize();
        Songs._yOA();
        Packs.OnAfterDeserialize();
    }

    private static Action CaptureText<T>(DynamicStringMapping.StringTypeMapping<T> mapping) {
        if (mapping?.Ids is null || mapping.IdValues is null)
            throw new InvalidOperationException("The library's text mappings are unavailable.");
        T[] ids = new T[mapping.Ids.Count];
        DynamicStringMapping.TextMappingValues[] values = new DynamicStringMapping.TextMappingValues[mapping.IdValues.Count];
        string[] text = mapping.IdStr is null ? null : new string[mapping.IdStr.Count];
        for (int index = 0; index < ids.Length; index++) ids[index] = mapping.Ids[index];
        for (int index = 0; index < values.Length; index++) values[index] = mapping.IdValues[index];
        if (text is not null)
            for (int index = 0; index < text.Length; index++) text[index] = mapping.IdStr[index];
        return () => {
            mapping.Ids.Clear();
            foreach (T id in ids) mapping.Ids.Add(id);
            mapping.IdValues.Clear();
            foreach (DynamicStringMapping.TextMappingValues value in values) {
#if BEPINEX
                NativeTextMapping.StoreValue(mapping.IdValues, value);
#else
                mapping.IdValues.Add(value);
#endif
            }
            if (text is not null) {
                mapping.IdStr.Clear();
                foreach (string value in text) mapping.IdStr.Add(value);
            }
        };
    }
}
