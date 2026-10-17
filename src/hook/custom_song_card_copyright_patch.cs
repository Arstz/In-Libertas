using System;
using HarmonyLib;
using Il2Cpp_K;
using Il2Cppifapp.Game.Data;
using Il2Cppifapp.Game.Scenes.SongSelect;

namespace InFalsusCustomSongHook;

[HarmonyPatch(typeof(LargeSongCard), nameof(LargeSongCard._OS))]
internal static class CustomSongCardCopyrightPatch
{
    [HarmonyPrefix]
    private static void Prefix(ref _SH __0)
    {
        try
        {
            SongInfo song = __0?._WEb;
            if (song is null || CustomSongInstaller.FindActiveSongByBaseName(song.BaseName) is null)
                return;

            int slot = (byte)__0._xEb switch
            {
                1 => 0,
                2 => 1,
                4 => 2,
                8 => 3,
                _ => -1,
            };
            var charts = song.ChartInfos;
            SongChartInfo chart = slot >= 0 && charts is not null && slot < charts.Length
                ? charts[slot] : null;
            CustomSongConfig config = chart?.Available == true
                ? CustomSongInstaller.FindActiveSongByChartKey(chart.Id + ".spc") : null;
            string copyright = config?.ChartDesigner ?? string.Empty;
            song.Copyright = copyright.Length == 1 ? copyright + "\u200B" : copyright;
            __0._WEb = song;
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Warning("[CustomSong] song-card copyright refresh failed: " + exception.Message);
        }
    }
}
