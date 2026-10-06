using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using HarmonyLib;
using Il2Cpp_J;
using Il2CppFMOD;

namespace InFalsusCustomSongHook;

/// <summary>
/// Lets the normal audio manager create a _Cg using the template request, then
/// replaces that _Cg's source and (when already allocated) its paused channel
/// with an external-WAV Sound created from the same running FMOD System. This
/// deliberately calls createSound; it does not Harmony-patch that generated
/// wrapper, which stalls game startup.
/// </summary>
[HarmonyPatch(typeof(_zf), nameof(_zf._RFA))]
internal static class CustomSongAudioOverride
{
    // Normal game streaming uses this exact FMOD MODE value for the categories
    // used by song preview and gameplay.
    // The game mode above includes FMOD_NONBLOCKING (0x00010000). A custom
    // controller needs its paused channel before the transition emits its
    // one-shot unpause signal, so open only the external WAV synchronously.
    private const int kExternalStreamMode = 167772296;
    // GameAssembly's shared _Cg factory stores its FMOD_CHANNEL* at +0x10 and
    // raw FMOD_SOUND* at +0x20. The paused flag it passes to System::playSound
    // is at +0x4c. Preserving these controller-owned fields lets the game
    // release/unpause the replacement at its normal transition point.
    private const int kNativeChannelOffset = 0x10;
    private const int kNativeSoundOffset = 0x20;
    private const int kNativePausedOffset = 0x4c;
    private const int kNativeSelectorCallbackOffset = 0x40;
    private const int kNativeConfiguredOffset = 0x58;
    private const int kNativePreviewStartOffset = 0x30;
    private const int kNativePreviewEndOffset = 0x38;
    private const int kNativeSkipPreviewSeekOffset = 0x54;
    private const int kFmodLoopOff = 1;
    private const int kFmodLoopNormal = 2;
    private const uint kFmodTimeUnitMs = 1;
    private static readonly object Gate = new();
    private static readonly List<PendingController> Pending = new();
    // Harmony's generated IL2CPP trampoline invokes our prefix reliably, but
    // does not preserve a value-type __state argument for this native method.
    // Keep the route record on the originating thread instead.  A stack makes
    // this correct even if the native audio factory nests an _RFA call.
    [ThreadStatic] private static Stack<RouteState> _routes;
    private static SoundGetSystemObjectDelegate _getSystemObject;
    private static SystemPlaySoundDelegate _playSound;
    private static ChannelGetChannelGroupDelegate _getChannelGroup;
    private static ChannelSetLoopCountDelegate _setLoopCount;
    private static ChannelSetModeDelegate _setMode;
    private static ChannelSetLoopPointsDelegate _setLoopPoints;
    private static ChannelSetPositionDelegate _setPosition;
    private static ChannelStopDelegate _stopChannel;

    [HarmonyPrefix]
    private static void Prefix(ref string __0, bool __6)
    {
        RouteState route = new();
        (_routes ??= new Stack<RouteState>()).Push(route);

        CustomSongConfig config = CustomSongInstaller.FindActiveSongByBaseName(__0);
        if (config is null || config.IsCarrierChartOverride)
            return;

        string templateBaseName = CustomSongInstaller.GetTemplateBaseName(config);
        if (string.IsNullOrWhiteSpace(templateBaseName)) return;

        __0 = templateBaseName;
        route.IsCustom = true;
        route.Config = config;
        // _RFA's seventh argument distinguishes the two native call sites:
        // song-select preview passes true; GameScene gameplay passes false.
        route.IsSelection = __6;
    }

    [HarmonyPostfix]
    private static void Postfix(_Cg __result)
    {
        if (_routes is null || _routes.Count == 0) return;

        RouteState route = _routes.Pop();
        if (!route.IsCustom) return;
        if (__result is null)
        {
            CustomSongMod.Log.Warning("[CustomSong] custom audio factory returned a null controller.");
            return;
        }

        lock (Gate)
        {
            foreach (PendingController pending in Pending)
            {
                if (pending.Controller.Pointer == __result.Pointer) return;
            }
            Pending.Add(new PendingController(__result, route.IsSelection, route.Config));
        }
    }

    internal static void Tick()
    {
        lock (Gate)
        {
            for (int index = Pending.Count - 1; index >= 0; index--)
            {
                PendingController pending = Pending[index];
                _Cg cg = pending.Controller;
                if (cg is null || cg.Pointer == IntPtr.Zero)
                {
                    Pending.RemoveAt(index);
                    continue;
                }
                if (GetNativeSound(cg) == IntPtr.Zero) continue;

                try
                {
                    ReplaceSound(cg, pending.IsSelection, pending.Config);
                }
                catch (Exception exception)
                {
                    CustomSongMod.Log.Error("[CustomSong] external WAV replacement failed: " + exception);
                }
                Pending.RemoveAt(index);
            }
        }
    }

    private static void ReplaceSound(_Cg cg, bool isSelection, CustomSongConfig config)
    {
        IntPtr templateSound = GetNativeSound(cg);
        Il2CppFMOD.System fmodSystem = GetSystem(templateSound);
        CREATESOUNDEXINFO exInfo = default;
        exInfo.cbsize = 224;
        exInfo.ignoresetfilesystem = 1;
        RESULT result = fmodSystem.createSound(
            config.AudioPath,
            (MODE)kExternalStreamMode,
            ref exInfo,
            out Sound externalSound);

        if ((int)result != 0 || externalSound.handle == IntPtr.Zero)
            throw new InvalidOperationException($"FMOD createSound returned {result}; handle={externalSound.handle}");

        // A zero flag means the factory deferred its normal configure/callback
        // pass, which will consume the replacement on its own.  A set flag
        // means a cached template completed that pass before our postfix; the
        // replacement needs the exact same native pass replayed below.
        bool wasConfigured = Marshal.ReadByte(IntPtr.Add(cg.Pointer, kNativeConfiguredOffset)) != 0;
        IntPtr originalChannel = GetNativeChannel(cg);
        SetNativeSound(cg, externalSound.handle);

        // A deferred factory result has no channel when the gameplay cutscene
        // sends its one-shot unpause signal. Prime the external channel now in
        // the exact paused state the factory requested and publish it through
        // _Cg + 0x10. The normal game controller then owns that channel.
        _playSound ??= ResolveExport<SystemPlaySoundDelegate>("FMOD5_System_PlaySound");
        _getChannelGroup ??= ResolveExport<ChannelGetChannelGroupDelegate>("FMOD5_Channel_GetChannelGroup");
        _setLoopCount ??= ResolveExport<ChannelSetLoopCountDelegate>("FMOD5_Channel_SetLoopCount");
        _setMode ??= ResolveExport<ChannelSetModeDelegate>("FMOD5_Channel_SetMode");
        _stopChannel ??= ResolveExport<ChannelStopDelegate>("FMOD5_Channel_Stop");

        IntPtr channelGroup = IntPtr.Zero;
        if (originalChannel != IntPtr.Zero)
        {
            int groupStatus = _getChannelGroup(originalChannel, out channelGroup);
            if (groupStatus != 0 || channelGroup == IntPtr.Zero)
                throw new InvalidOperationException($"FMOD Channel::getChannelGroup returned {groupStatus}; channel={originalChannel}");
        }

        int paused = Marshal.ReadByte(IntPtr.Add(cg.Pointer, kNativePausedOffset)) != 0 ? 1 : 0;
        int playStatus = _playSound(fmodSystem.handle, externalSound.handle, channelGroup, paused, out IntPtr replacementChannel);
        if (playStatus != 0 || replacementChannel == IntPtr.Zero)
            throw new InvalidOperationException($"FMOD System::playSound returned {playStatus}; channel={replacementChannel}");

        int modeStatus = _setMode(replacementChannel, kFmodLoopOff);
        int loopStatus = _setLoopCount(replacementChannel, 0);
        if (modeStatus != 0 || loopStatus != 0)
            throw new InvalidOperationException($"FMOD one-shot setup failed: SetMode={modeStatus}; SetLoopCount={loopStatus}");

        SetNativeChannel(cg, replacementChannel);
        if (originalChannel != IntPtr.Zero)
        {
            int stopStatus = _stopChannel(originalChannel);
            if (stopStatus != 0)
                throw new InvalidOperationException($"FMOD Channel::stop returned {stopStatus}; channel={originalChannel}");
        }

        if (isSelection && wasConfigured)
            ReplayCompletedSelectionSetup(cg, replacementChannel);
    }

    private static void ReplayCompletedSelectionSetup(_Cg cg, IntPtr replacementChannel)
    {
        IntPtr callbackPointer = Marshal.ReadIntPtr(IntPtr.Add(cg.Pointer, kNativeSelectorCallbackOffset));
        if (callbackPointer == IntPtr.Zero)
            throw new InvalidOperationException("cached selection controller has no native continuation");

        // 0x18044A6A0 normally performs this exact FMOD setup before calling
        // the selector continuation. It cannot safely be re-entered after its
        // immediate branch has retired its queue bookkeeping, so reproduce
        // just its documented channel operations against the replacement.
        double startSeconds = BitConverter.Int64BitsToDouble(
            Marshal.ReadInt64(IntPtr.Add(cg.Pointer, kNativePreviewStartOffset)));
        double endSeconds = BitConverter.Int64BitsToDouble(
            Marshal.ReadInt64(IntPtr.Add(cg.Pointer, kNativePreviewEndOffset)));
        uint startMs = SecondsToMilliseconds(startSeconds);
        uint endMs = SecondsToMilliseconds(endSeconds);

        _setLoopPoints ??= ResolveExport<ChannelSetLoopPointsDelegate>("FMOD5_Channel_SetLoopPoints");
        _setPosition ??= ResolveExport<ChannelSetPositionDelegate>("FMOD5_Channel_SetPosition");
        int loopCountStatus = _setLoopCount(replacementChannel, -1);
        int loopPointsStatus = _setLoopPoints(
            replacementChannel, startMs, kFmodTimeUnitMs, endMs, kFmodTimeUnitMs);
        int loopModeStatus = _setMode(replacementChannel, kFmodLoopNormal);
        if (loopCountStatus != 0 || loopPointsStatus != 0 || loopModeStatus != 0)
            throw new InvalidOperationException(
                $"native preview channel setup failed: count={loopCountStatus}; points={loopPointsStatus}; mode={loopModeStatus}");

        if (Marshal.ReadByte(IntPtr.Add(cg.Pointer, kNativeSkipPreviewSeekOffset)) == 0)
        {
            int seekStatus = _setPosition(replacementChannel, startMs, kFmodTimeUnitMs);
            if (seekStatus != 0)
                throw new InvalidOperationException($"native preview seek failed: status={seekStatus}; startMs={startMs}");
        }

        new Il2CppSystem.Action<_Cg>(callbackPointer).Invoke(cg);
    }

    private static uint SecondsToMilliseconds(double seconds)
    {
        if (double.IsNaN(seconds) || seconds <= 0.0) return 0;
        if (double.IsInfinity(seconds) || seconds >= uint.MaxValue / 1000.0) return uint.MaxValue;
        return (uint)(seconds * 1000.0);
    }

    private static IntPtr GetNativeSound(_Cg cg)
    {
        return Marshal.ReadIntPtr(IntPtr.Add(cg.Pointer, kNativeSoundOffset));
    }

    private static IntPtr GetNativeChannel(_Cg cg)
    {
        return Marshal.ReadIntPtr(IntPtr.Add(cg.Pointer, kNativeChannelOffset));
    }

    private static void SetNativeSound(_Cg cg, IntPtr sound)
    {
        Marshal.WriteIntPtr(IntPtr.Add(cg.Pointer, kNativeSoundOffset), sound);
    }

    private static void SetNativeChannel(_Cg cg, IntPtr channel)
    {
        Marshal.WriteIntPtr(IntPtr.Add(cg.Pointer, kNativeChannelOffset), channel);
    }

    private static Il2CppFMOD.System GetSystem(IntPtr sound)
    {
        lock (Gate)
        {
            _getSystemObject ??= ResolveGetSystemObject();
            int status = _getSystemObject(sound, out IntPtr handle);
            if (status != 0 || handle == IntPtr.Zero)
                throw new InvalidOperationException($"FMOD Sound::getSystemObject returned {status}; source={sound}; system={handle}");
            return new Il2CppFMOD.System { handle = handle };
        }
    }

    private static SoundGetSystemObjectDelegate ResolveGetSystemObject()
    {
        // The copied FMOD databases contain this C ABI export. Resolve it at
        // runtime just as the game's native bridge resolves FMOD5 callbacks;
        // do not call a C++ member-function RVA against an opaque handle.
        IntPtr module = GetModuleHandle("fmodstudioL.dll");
        if (module == IntPtr.Zero) module = GetModuleHandle("fmodstudio.dll");
        if (module == IntPtr.Zero)
            throw new InvalidOperationException("FMOD runtime module is not loaded");
        return ResolveExport<SoundGetSystemObjectDelegate>("FMOD5_Sound_GetSystemObject", module);
    }

    private static T ResolveExport<T>(string exportName) where T : Delegate
    {
        IntPtr module = GetModuleHandle("fmodstudioL.dll");
        if (module == IntPtr.Zero) module = GetModuleHandle("fmodstudio.dll");
        if (module == IntPtr.Zero)
            throw new InvalidOperationException("FMOD runtime module is not loaded");
        return ResolveExport<T>(exportName, module);
    }

    private static T ResolveExport<T>(string exportName, IntPtr module) where T : Delegate
    {
        IntPtr address = GetProcAddress(module, exportName);
        if (address == IntPtr.Zero)
            throw new InvalidOperationException(exportName + " export is unavailable");
        return Marshal.GetDelegateForFunctionPointer<T>(address);
    }

    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string moduleName);

    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SoundGetSystemObjectDelegate(IntPtr sound, out IntPtr system);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SystemPlaySoundDelegate(IntPtr system, IntPtr sound, IntPtr channelGroup, int paused, out IntPtr channel);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelGetChannelGroupDelegate(IntPtr channel, out IntPtr channelGroup);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelSetLoopCountDelegate(IntPtr channel, int loopCount);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelSetModeDelegate(IntPtr channel, int mode);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelSetLoopPointsDelegate(
        IntPtr channel,
        uint loopStart,
        uint loopStartType,
        uint loopEnd,
        uint loopEndType);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelSetPositionDelegate(IntPtr channel, uint position, uint timeUnit);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelStopDelegate(IntPtr channel);

    private sealed class PendingController
    {
        internal PendingController(_Cg controller, bool isSelection, CustomSongConfig config)
        {
            Controller = controller;
            IsSelection = isSelection;
            Config = config;
        }

        internal _Cg Controller { get; }
        internal bool IsSelection { get; }
        internal CustomSongConfig Config { get; }
    }

    private sealed class RouteState
    {
        internal bool IsCustom;
        internal bool IsSelection;
        internal CustomSongConfig Config;
    }
}
