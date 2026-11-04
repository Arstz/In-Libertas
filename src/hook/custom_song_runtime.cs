using System;
using System.IO;
using System.Reflection;
using HarmonyLib;

namespace InFalsusCustomSongHook;

internal sealed class HookLogger {
    private readonly Action<string> m_message;
    private readonly Action<string> m_warning;
    private readonly Action<string> m_error;

    internal HookLogger(Action<string> message, Action<string> warning, Action<string> error) {
        m_message = message;
        m_warning = warning;
        m_error = error;
    }

    internal void Msg(string message) => m_message(message);
    internal void Warning(string message) => m_warning(message);
    internal void Error(string message) => m_error(message);
}

internal static class CustomSongRuntime {
    internal const string Version = "0.2.0";
    internal static HookLogger Log { get; private set; }
    internal static string GameRoot { get; private set; }
    private static HarmonyLib.Harmony m_harmony;
    private static ProjectImporter m_importer;
    private static CustomLibraryWatcher m_watcher;
    private static bool m_hooksInstalled;

    internal static void Initialize(string gameRoot, HookLogger logger, HarmonyLib.Harmony harmony) {
        GameRoot = gameRoot;
        Log = logger;
        m_harmony = harmony;
#if MELONLOADER
        CustomSongInstaller.PrepareStartupMethods();
#endif
        PrepareProjects();
        m_watcher = new CustomLibraryWatcher(CustomSongConfig.CustomChartsRoot,
            message => Log.Warning("[CustomSong] " + message));
#if MELONLOADER
        InstallHooks();
#endif
        Log.Msg("[CustomSong] loaded; waiting for game data to become ready.");
    }

    private static void InstallHooks() {
        m_harmony.PatchAll(Assembly.GetExecutingAssembly());
        CustomSongTitleLocalizationPatch.Install();
        ChartEventBufferPatch.Install();
        NativeJacketResolverOverride.Install();
        m_hooksInstalled = true;
    }

    internal static void Update() {
        if (!m_hooksInstalled) InstallHooks();
        if (!CustomSongInstaller.IsComplete) CustomSongInstaller.ProbeForReadyData();
        if (m_importer is not null && CustomSongInstaller.ActiveConfig?.IsLibrary == true) {
            m_importer.RecyclePending();
            m_importer = null;
        }
        CustomSongInstaller.Tick();
        CustomJacketOverride.Tick();
        CustomSongAudioOverride.Tick();
    }

    internal static void RefreshLibrary() {
        try {
            if (m_watcher is null || !m_watcher.HasChanges()) return;
            PrepareProjects();
            m_watcher.Checkpoint();
            if (!CustomSongConfig.TryLoadForReload(out CustomSongConfig config, out string message))
                throw new InvalidOperationException(message);
            if (!m_watcher.IsCurrentCheckpoint())
                throw new IOException("The chart library changed while it was being read; reload will retry on the next visit.");
            CustomSongInstaller.ReloadLibrary(config, m_watcher.IsCurrentCheckpoint);
            m_importer?.RecyclePending();
            m_importer = null;
            m_watcher.AcceptChanges();
            Log.Msg("[CustomSong] library reloaded: " + message);
        } catch (Exception exception) {
            m_importer = null;
            Log.Error("[CustomSong] library reload failed; retaining the installed library: " + exception);
        }
    }

    private static void PrepareProjects() {
        if (!OperatingSystem.IsWindows()) return;
        try {
            m_importer = new ProjectImporter(CustomSongConfig.CustomChartsRoot,
                message => Log.Msg("[CustomSong] " + message), ProjectRecycler.Recycle, deferRecycling: true);
            m_importer.Prepare();
        } catch (Exception exception) {
            Log.Warning("[CustomSong] project preparation failed: " + exception.Message);
        }
    }

    internal static void Shutdown() {
        m_watcher?.Dispose();
        m_watcher = null;
        m_harmony?.UnpatchSelf();
        m_hooksInstalled = false;
        CustomSongAudioOverride.Dispose();
        CustomSongInstaller.RestoreLibrary();
        ChartEventBufferPatch.Dispose();
        CustomSongTitleLocalizationPatch.Dispose();
        CustomPackJacketOverride.Dispose();
        CustomJacketOverride.Dispose();
        CustomChartOverride.Reset();
        NativeRuntimeOverrideBridge.Dispose();
    }
}
