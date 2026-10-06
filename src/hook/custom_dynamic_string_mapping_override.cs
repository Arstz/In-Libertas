using System;
using HarmonyLib;
using Il2Cppifapp.Game.Data;

namespace InFalsusCustomSongHook;

/// <summary>
/// Keeps the custom SongId text entries alive if Unity deserializes the shared
/// string mapping after the startup library extension has completed.
/// </summary>
[HarmonyPatch(typeof(DynamicStringMapping), nameof(DynamicStringMapping.OnAfterDeserialize))]
internal static class CustomDynamicStringMappingOverride
{
    [HarmonyPostfix]
    private static void Postfix(DynamicStringMapping __instance)
    {
        try
        {
            CustomSongInstaller.ReapplyLibraryDisplayMetadata(__instance);
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Warning("[CustomSong] custom title/artist mapping refresh failed: " + exception.Message);
        }
    }
}
