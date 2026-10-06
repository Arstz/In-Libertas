using HarmonyLib;
using Il2Cppifapp.Game;

namespace InFalsusCustomSongHook;

/// <summary>
/// PackSelectScene reads its parallel visual-style table during Awake.  Add
/// styles before that read, using the collection's configured stock theme.
/// </summary>
[HarmonyPatch(typeof(PackSelectScene), nameof(PackSelectScene.Awake))]
internal static class CustomCollectionAssetsPatch
{
    [HarmonyPrefix]
    private static void Prefix(PackSelectScene __instance)
    {
        // The scene is the final safe opportunity before it snapshots the
        // parallel pack-style array. It also guarantees DataAccess exists.
        if (!CustomSongInstaller.IsComplete)
            CustomSongInstaller.ProbeForReadyData();
        CustomSongInstaller.ApplyCollectionAssets(__instance?.packSelectSceneAssets);
    }
}
