using HarmonyLib;
using Il2Cppifapp.Game.Data;

namespace InFalsusCustomSongHook;

/// <summary>
/// The native cache constructor uses the current allSongInfo length for its
/// tutorial BitArray. This prefix is the authoritative install boundary for
/// the expandable library; frame-based probing is only a fallback.
/// </summary>
[HarmonyPatch(typeof(SongData), nameof(SongData._yOA))]
internal static class CustomSongDataCachePatch
{
    [HarmonyPrefix]
    private static void Prefix(SongData __instance) =>
        CustomSongInstaller.TryInstallBeforeSelectorCache(__instance);
}
