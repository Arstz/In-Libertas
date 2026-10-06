using System;
using HarmonyLib;
using Il2Cpp_k;

namespace InFalsusCustomSongHook;

/// <summary>
/// The streaming mount snapshots its logical-path list into _BG._txA during
/// startup.  A SongChartInfo added later therefore cannot resolve a new key
/// by merely appending a StreamingAssetsMapping entry.  Redirect the one
/// custom logical key at that cache boundary to the template key which was
/// present when the mount was built.  The caller retains the original custom
/// key and supplies it to _S._Gab, where CustomChartOverride replaces the
/// source bytes with the external ICP1 payload.
/// </summary>
[HarmonyPatch(typeof(_BG), nameof(_BG._DJA))]
internal static class CustomChartPathOverride
{
    private static bool _reported;

    [HarmonyPrefix]
    private static void Prefix(ref string __0)
    {
        CustomSongConfig config = CustomSongInstaller.FindActiveSongByChartKey(__0);
        string templateKey = CustomSongInstaller.GetTemplateChartKey(config);
        if (config is null || config.IsCarrierChartOverride || string.IsNullOrWhiteSpace(__0) ||
            string.IsNullOrWhiteSpace(templateKey) ||
            !string.Equals(__0, config.ChartKey, StringComparison.OrdinalIgnoreCase))
            return;

        string customKey = __0;
        __0 = templateKey;
        if (_reported) return;
        _reported = true;
        CustomSongMod.Log.Msg($"[CustomSong] chart path redirected: {customKey} -> {templateKey}");
    }
}
