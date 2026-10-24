using HarmonyLib;
using Il2Cppifapp.Game.Data;
using Il2CppStr;

namespace InFalsusCustomSongHook;

[HarmonyPatch(typeof(DynamicStringMapping.TextMappingValues), nameof(DynamicStringMapping.TextMappingValues._WLA))]
internal static class CustomSongTitleLocalizationPatch
{
    [HarmonyPostfix]
    private static void Postfix(DynamicStringMapping.TextMappingValues __instance, Strings.Localization __0, ref string __result) {
        if (__0 == Strings.Localization.Korean && !string.IsNullOrEmpty(__instance.Korean))
            __result = __instance.Korean;
    }
}
