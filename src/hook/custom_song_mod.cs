#if MELONLOADER
using MelonLoader;

[assembly: MelonInfo(typeof(InFalsusCustomSongHook.CustomSongMod), "In Falsus Custom Song Hook", InFalsusCustomSongHook.CustomSongRuntime.Version, "Arstz")]
[assembly: MelonGame("lowiro", "infalsus")]

namespace InFalsusCustomSongHook;

public sealed class CustomSongMod : MelonMod {
    internal static HookLogger Log => CustomSongRuntime.Log;

    public override void OnInitializeMelon() => CustomSongRuntime.Initialize(
        MelonLoader.Utils.MelonEnvironment.GameRootDirectory,
        new HookLogger(message => LoggerInstance.Msg(message),
            message => LoggerInstance.Warning(message), message => LoggerInstance.Error(message)), HarmonyInstance);

    public override void OnUpdate() => CustomSongRuntime.Update();

    public override void OnDeinitializeMelon() => CustomSongRuntime.Shutdown();
}
#else
namespace InFalsusCustomSongHook;

internal static class CustomSongMod {
    internal static HookLogger Log => CustomSongRuntime.Log;
}
#endif
