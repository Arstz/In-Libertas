using System.Reflection;
using MelonLoader;

[assembly: MelonInfo(typeof(InFalsusCustomSongHook.CustomSongMod), "In Falsus Custom Song Hook", "0.1.2", "InFalsusDump")]
[assembly: MelonGame("lowiro", "infalsus")]

namespace InFalsusCustomSongHook;

public sealed class CustomSongMod : MelonMod
{
    internal static MelonLogger.Instance Log { get; private set; } = null;

    public override void OnInitializeMelon()
    {
        Log = LoggerInstance;
        HarmonyInstance.PatchAll(Assembly.GetExecutingAssembly());
        NativeJacketResolverOverride.Install();
        Log.Msg("[CustomSong] loaded; waiting for game data to become ready.");
    }

    public override void OnUpdate()
    {
        // A library resize is safe only before SongData builds _Ffb. Probe on
        // every startup frame rather than waiting for the old 30-frame cadence.
        if (!CustomSongInstaller.IsComplete)
            CustomSongInstaller.ProbeForReadyData();
        CustomSongInstaller.Tick();
        CustomJacketOverride.Tick();

        CustomSongAudioOverride.Tick();
    }

    public override void OnDeinitializeMelon() => CustomJacketOverride.Dispose();
}
