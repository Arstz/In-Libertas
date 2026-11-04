using HarmonyLib;
#if BEPINEX
using ifapp.Game.Scenes;
#else
using Il2Cppifapp.Game.Scenes;
#endif

namespace InFalsusCustomSongHook;

[HarmonyPatch(typeof(SongSelectScene), nameof(SongSelectScene.Awake))]
internal static class CustomLibraryReloadPatch {
    [HarmonyPrefix]
    private static void Prefix() => CustomSongRuntime.RefreshLibrary();
}
