using System.Reflection;
using MelonLoader;

[assembly: MelonInfo(typeof(InFalsusCustomSongHook.CustomSongMod), "In Falsus Custom Song Hook", "0.1.2", "InFalsusDump")]
[assembly: MelonGame("lowiro", "infalsus")]

namespace InFalsusCustomSongHook;

public sealed class CustomSongMod : MelonMod
{
    private ProjectImporter m_projectImporter;
    internal static MelonLogger.Instance Log { get; private set; } = null;

    public override void OnInitializeMelon()
    {
        Log = LoggerInstance;
        Log.Msg("[CustomSong] installer JIT preparation starting before project import.");
        CustomSongInstaller.PrepareStartupMethods();
        Log.Msg("[CustomSong] installer JIT preparation complete; starting project preparation.");
        try
        {
            if (System.OperatingSystem.IsWindows()) {
                m_projectImporter = new ProjectImporter(CustomSongConfig.CustomChartsRoot,
                    message => Log.Msg("[CustomSong] " + message), ProjectRecycler.Recycle, deferRecycling: true);
                m_projectImporter.Prepare();
            }
        }
        catch (System.Exception exception)
        {
            Log.Warning("[CustomSong] project preparation failed; existing folders remain available: " + exception.Message);
        }
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
        RecyclePreparedProjects();
        CustomSongInstaller.Tick();
        CustomJacketOverride.Tick();

        CustomSongAudioOverride.Tick();
    }

    private void RecyclePreparedProjects() {
        if (m_projectImporter is null || !CustomSongInstaller.IsComplete || CustomSongInstaller.ActiveConfig?.IsLibrary != true)
            return;
        ProjectImporter importer = m_projectImporter;
        m_projectImporter = null;
        Log.Msg("[CustomSong] library installation complete; processing deferred recycling.");
        importer.RecyclePending();
    }

    public override void OnDeinitializeMelon() {
        CustomSongAudioOverride.Dispose();
        CustomJacketOverride.Dispose();
    }
}
