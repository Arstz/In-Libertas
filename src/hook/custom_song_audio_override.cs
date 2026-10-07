using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using HarmonyLib;
using Il2CppInterop.Runtime;
using Il2Cpp_J;
using Il2CppFMOD;

namespace InFalsusCustomSongHook;

[HarmonyPatch(typeof(_zf), nameof(_zf._RFA))]
internal static class CustomSongAudioOverride {
    private const int kExternalStreamMode = 167772296;
    private const int kCreateSoundInfoSize = 224;
    private const long kPreviewTimeoutMilliseconds = 30000;
    private const int kNativeChannelOffset = 0x10;
    private const int kNativeChannelUserDataOffset = 0x18;
    private const int kNativeSoundOffset = 0x20;
    private const int kNativeChannelCategoryOffset = 0x48;
    private const int kNativePausedOffset = 0x4c;
    private const int kNativePreviewLoopOffset = 0x4d;
    private const int kNativePreviewFadeOffset = 0x4e;
    private const int kNativePreviewStartOffset = 0x30;
    private const int kNativePreviewEndOffset = 0x38;
    private const int kNativeSkipPreviewSeekOffset = 0x54;
    private const int kNativeDisposedOffset = 0xa0;
    private const int kNativeDisposingOffset = 0xa1;
    private const float kInitialChannelVolume = 1.0f;
    private static readonly object m_gate = new();
    private static readonly List<PendingController> m_pending = new();
    private static readonly List<OwnedSound> m_ownedSounds = new();
    [ThreadStatic] private static Stack<RouteState> m_routes;
    private static string m_latestPreviewBaseName;
    private static long m_previewGeneration;
    private static SoundGetSystemObjectDelegate m_getSystemObject;
    private static ChannelGetCurrentSoundDelegate m_getCurrentSound;
    private static ChannelStopDelegate m_stopChannel;
    private static ChannelIsPlayingDelegate m_isPlaying;
    private static SoundGetOpenStateDelegate m_getOpenState;
    private static SoundReleaseDelegate m_releaseSound;

    [HarmonyPrefix]
    private static void Prefix(ref string __0, ref Il2CppSystem.Action<_Cg> __2, ref bool __3, bool __6) {
        RouteState route = new();
        CustomSongConfig config = CustomSongInstaller.FindActiveSongByBaseName(__0);
        bool isSelection = __3 && __6;
        bool isRootRequest = m_routes is null || m_routes.Count == 0;

        (m_routes ??= new Stack<RouteState>()).Push(route);
        lock (m_gate) {
            if (isSelection && isRootRequest) {
                ++m_previewGeneration;
                m_latestPreviewBaseName = __0;
                CustomSongMod.Log.Msg($"[CustomSong] audio preview request: {__0}; generation={m_previewGeneration}");
            }
            route.Generation = m_previewGeneration;
            if (!isRootRequest && __6)
                CustomSongMod.Log.Msg($"[CustomSong] nested audio factory route: {__0}; loop={__3}; fade={__6}; generation={route.Generation}");
        }
        if (config is null || config.IsCarrierChartOverride) return;

        string templateBaseName = CustomSongInstaller.GetTemplateBaseName(config);
        if (string.IsNullOrWhiteSpace(templateBaseName)) return;

        route.Config = config;
        route.IsSelection = isSelection;
        if (isSelection) {
            route.SelectorCallback = __2;
            route.FactoryCallback = DelegateSupport.ConvertDelegate<Il2CppSystem.Action<_Cg>>(
                new Action<_Cg>(controller => {
                    lock (m_gate) route.NativeReady = true;
                }));
            __2 = route.FactoryCallback;
            __3 = false;
        }
        __0 = templateBaseName;
    }

    [HarmonyPostfix]
    private static void Postfix(_Cg __result) {
        if (m_routes is null || m_routes.Count == 0) return;

        RouteState route = m_routes.Pop();
        if (route.Config is null) return;
        if (__result is null) {
            CustomSongMod.Log.Warning("[CustomSong] custom audio factory returned a null controller.");
            return;
        }

        lock (m_gate) {
            PendingController pending = new(__result, route);
            m_pending.Add(pending);
            if (!route.IsSelection) return;
            try {
                OpenPreview(pending);
            } catch (Exception exception) {
                FailPending(pending, exception);
                m_pending.Remove(pending);
            }
        }
    }

    internal static void Tick() {
        lock (m_gate) {
            for (int index = m_pending.Count - 1; index >= 0; --index) {
                PendingController pending = m_pending[index];
                try {
                    if (IsRetired(pending.Controller)) {
                        if (pending.Route.IsSelection)
                            CustomSongMod.Log.Msg($"[CustomSong] audio preview retired before ready: {pending.Route.Config.ChartId}; generation={pending.Route.Generation}");
                        m_pending.RemoveAt(index);
                        continue;
                    }
                    if (pending.Route.IsSelection && pending.Route.Generation != m_previewGeneration) {
                        RetireController(pending.Controller);
                        CustomSongMod.Log.Msg($"[CustomSong] superseded audio preview: {pending.Route.Config.ChartId}; generation={pending.Route.Generation}; activeGeneration={m_previewGeneration}; activeSong={m_latestPreviewBaseName}");
                        m_pending.RemoveAt(index);
                        continue;
                    }
                    if (GetNativeSound(pending.Controller) == IntPtr.Zero) continue;
                    if (pending.Route.IsSelection) {
                        if (!AdvancePreview(pending)) continue;
                    } else {
                        ReplaceGameplaySound(pending);
                    }
                    m_pending.RemoveAt(index);
                } catch (Exception exception) {
                    FailPending(pending, exception);
                    m_pending.RemoveAt(index);
                }
            }
            ReleaseRetiredSounds();
        }
    }

    private static void OpenPreview(PendingController pending) {
        _Cg controller = pending.Controller;
        IntPtr templateSound = GetNativeSound(controller);
        if (IsRetired(controller) || templateSound == IntPtr.Zero) return;

        pending.OriginalChannel = GetNativeChannel(controller);
        pending.Sound = OpenSound(controller, pending.Route, templateSound, true);
        SetNativeSound(controller, pending.Sound.Handle);
        CustomSongMod.Log.Msg($"[CustomSong] audio preview loading: {pending.Route.Config.ChartId}; generation={pending.Route.Generation}");
    }

    private static bool AdvancePreview(PendingController pending) {
        _Cg controller = pending.Controller;
        if (pending.Elapsed.ElapsedMilliseconds >= kPreviewTimeoutMilliseconds)
            throw new TimeoutException("external audio preview did not become ready");
        if (pending.Sound is null) OpenPreview(pending);
        if (pending.Sound is null) return false;
        if (!IsSoundReady(pending.Sound.Handle) || !pending.Route.NativeReady) return false;

        if (!pending.SeekIssued) {
            IntPtr channel = pending.OriginalChannel == IntPtr.Zero
                ? GetNativeChannel(controller) : ReplaceChannel(controller, pending.Sound, pending.OriginalChannel);
            m_getCurrentSound ??= ResolveExport<ChannelGetCurrentSoundDelegate>("FMOD5_Channel_GetCurrentSound");
            int currentSoundStatus = m_getCurrentSound(channel, out IntPtr channelSound);
            if (currentSoundStatus != 0 || channelSound != pending.Sound.Handle)
                throw new InvalidOperationException("preview channel does not own the external sound");

            pending.Sound.Channel = channel;
            ConfigurePreview(controller, channel);
            pending.SeekIssued = true;

            return false;
        }

        Marshal.WriteByte(IntPtr.Add(controller.Pointer, kNativePreviewLoopOffset), 1);
        pending.Route.SelectorCallback?.Invoke(controller);
        CustomSongMod.Log.Msg($"[CustomSong] audio preview ready: {pending.Route.Config.ChartId}; generation={pending.Route.Generation}; elapsed={pending.Elapsed.ElapsedMilliseconds} ms");

        return true;
    }

    private static void ReplaceGameplaySound(PendingController pending) {
        _Cg controller = pending.Controller;
        OwnedSound sound = OpenSound(controller, pending.Route, GetNativeSound(controller), false);
        IntPtr originalChannel = GetNativeChannel(controller);

        SetNativeSound(controller, sound.Handle);
        ReplaceChannel(controller, sound, originalChannel);
    }

    private static OwnedSound OpenSound(_Cg controller, RouteState route, IntPtr templateSound, bool nonblocking) {
        Il2CppFMOD.System system = GetSystem(templateSound);
        CREATESOUNDEXINFO info = default;
        MODE mode = (MODE)kExternalStreamMode;

        info.cbsize = kCreateSoundInfoSize;
        info.ignoresetfilesystem = 1;
        if (nonblocking) mode |= MODE.NONBLOCKING;
        RESULT result = system.createSound(route.Config.AudioPath, mode, ref info, out Sound externalSound);
        if ((int)result != 0 || externalSound.handle == IntPtr.Zero)
            throw new InvalidOperationException($"FMOD createSound returned {result}; handle={externalSound.handle}");

        OwnedSound ownedSound = new(controller, route, externalSound.handle);
        m_ownedSounds.Add(ownedSound);

        return ownedSound;
    }

    private static IntPtr ReplaceChannel(_Cg controller, OwnedSound sound, IntPtr originalChannel) {
        Sound externalSound = new() { handle = sound.Handle };
        _IF._jF category = (_IF._jF)Marshal.ReadInt32(IntPtr.Add(controller.Pointer, kNativeChannelCategoryOffset));
        IntPtr userData = Marshal.ReadIntPtr(IntPtr.Add(controller.Pointer, kNativeChannelUserDataOffset));
        bool paused = Marshal.ReadByte(IntPtr.Add(controller.Pointer, kNativePausedOffset)) != 0;

        Channel replacementChannel = _IF._oGA(category, externalSound, paused, kInitialChannelVolume, userData);
        if (replacementChannel.handle == IntPtr.Zero)
            throw new InvalidOperationException("game audio factory returned a null channel");

        sound.Channel = replacementChannel.handle;
        SetNativeChannel(controller, replacementChannel.handle);
        if (originalChannel != IntPtr.Zero) {
            m_stopChannel ??= ResolveExport<ChannelStopDelegate>("FMOD5_Channel_Stop");
            m_stopChannel(originalChannel);
        }

        return replacementChannel.handle;
    }

    private static void ConfigurePreview(_Cg controller, IntPtr channel) {
        Channel previewChannel = new() { handle = channel };
        bool fade = Marshal.ReadByte(IntPtr.Add(controller.Pointer, kNativePreviewFadeOffset)) != 0;
        bool skipSeek = Marshal.ReadByte(IntPtr.Add(controller.Pointer, kNativeSkipPreviewSeekOffset)) != 0;
        double startSeconds = BitConverter.Int64BitsToDouble(
            Marshal.ReadInt64(IntPtr.Add(controller.Pointer, kNativePreviewStartOffset)));
        double endSeconds = BitConverter.Int64BitsToDouble(
            Marshal.ReadInt64(IntPtr.Add(controller.Pointer, kNativePreviewEndOffset)));

        _IF._RGA(previewChannel, startSeconds, endSeconds, fade);
        if (!skipSeek) _IF._sGA(previewChannel, startSeconds);
    }

    private static bool IsSoundReady(IntPtr sound) {
        m_getOpenState ??= ResolveExport<SoundGetOpenStateDelegate>("FMOD5_Sound_GetOpenState");
        int status = m_getOpenState(sound, out OPENSTATE state, out _, out _, out _);
        if (status == (int)RESULT.ERR_NOTREADY) return false;
        CheckResult(status, "Sound::getOpenState");
        if (state == OPENSTATE.ERROR) throw new InvalidOperationException("external audio stream failed to open");

        return state == OPENSTATE.READY || state == OPENSTATE.PLAYING;
    }

    private static bool IsRetired(_Cg controller) {
        return controller is null || controller.Pointer == IntPtr.Zero
            || Marshal.ReadByte(IntPtr.Add(controller.Pointer, kNativeDisposedOffset)) != 0
            || Marshal.ReadByte(IntPtr.Add(controller.Pointer, kNativeDisposingOffset)) != 0;
    }

    private static void RetireController(_Cg controller) {
        if (!IsRetired(controller)) controller.Dispose();
    }

    private static void FailPending(PendingController pending, Exception exception) {
        CustomSongMod.Log.Error($"[CustomSong] external audio replacement failed: {pending.Route.Config.ChartId}; {exception}");
        try {
            RetireController(pending.Controller);
        } catch (Exception retirementException) {
            CustomSongMod.Log.Warning("[CustomSong] audio controller retirement failed: " + retirementException.Message);
        }
    }

    private static void ReleaseRetiredSounds() {
        if (m_ownedSounds.Count == 0) return;

        m_getOpenState ??= ResolveExport<SoundGetOpenStateDelegate>("FMOD5_Sound_GetOpenState");
        m_releaseSound ??= ResolveExport<SoundReleaseDelegate>("FMOD5_Sound_Release");
        m_isPlaying ??= ResolveExport<ChannelIsPlayingDelegate>("FMOD5_Channel_IsPlaying");
        for (int index = m_ownedSounds.Count - 1; index >= 0; --index) {
            OwnedSound sound = m_ownedSounds[index];
            if (!IsRetired(sound.Controller) && GetNativeSound(sound.Controller) == sound.Handle) {
                IntPtr channel = GetNativeChannel(sound.Controller);
                if (ChannelOwnsSound(channel, sound.Handle)) sound.Channel = channel;
                continue;
            }
            if (ChannelOwnsSound(sound.Channel, sound.Handle)
                && m_isPlaying(sound.Channel, out int playing) == 0 && playing != 0) continue;

            int stateStatus = m_getOpenState(sound.Handle, out OPENSTATE state, out _, out _, out _);
            if (stateStatus == (int)RESULT.ERR_INVALID_HANDLE) {
                m_ownedSounds.RemoveAt(index);
                continue;
            }
            if (state != OPENSTATE.ERROR && (stateStatus != 0 || state != OPENSTATE.READY)) continue;
            int releaseStatus = m_releaseSound(sound.Handle);
            if (releaseStatus != 0)
                CustomSongMod.Log.Warning($"[CustomSong] external audio release returned {releaseStatus}: {sound.Route.Config.ChartId}");
            m_ownedSounds.RemoveAt(index);
        }
    }

    internal static void Dispose() {
        lock (m_gate) {
            foreach (OwnedSound sound in m_ownedSounds) {
                try {
                    RetireController(sound.Controller);
                    if (ChannelOwnsSound(sound.Channel, sound.Handle)) {
                        m_stopChannel ??= ResolveExport<ChannelStopDelegate>("FMOD5_Channel_Stop");
                        m_stopChannel(sound.Channel);
                    }
                } catch (Exception exception) {
                    CustomSongMod.Log.Warning("[CustomSong] audio shutdown failed: " + exception.Message);
                }
            }
            m_pending.Clear();
            ReleaseRetiredSounds();
        }
    }

    private static bool ChannelOwnsSound(IntPtr channel, IntPtr sound) {
        if (channel == IntPtr.Zero) return false;

        m_getCurrentSound ??= ResolveExport<ChannelGetCurrentSoundDelegate>("FMOD5_Channel_GetCurrentSound");

        return m_getCurrentSound(channel, out IntPtr currentSound) == 0 && currentSound == sound;
    }

    private static void CheckResult(int status, string operation) {
        if (status != 0) throw new InvalidOperationException($"FMOD {operation} returned {status}");
    }

    private static IntPtr GetNativeSound(_Cg controller) {
        return Marshal.ReadIntPtr(IntPtr.Add(controller.Pointer, kNativeSoundOffset));
    }

    private static IntPtr GetNativeChannel(_Cg controller) {
        return Marshal.ReadIntPtr(IntPtr.Add(controller.Pointer, kNativeChannelOffset));
    }

    private static void SetNativeSound(_Cg controller, IntPtr sound) {
        Marshal.WriteIntPtr(IntPtr.Add(controller.Pointer, kNativeSoundOffset), sound);
    }

    private static void SetNativeChannel(_Cg controller, IntPtr channel) {
        Marshal.WriteIntPtr(IntPtr.Add(controller.Pointer, kNativeChannelOffset), channel);
    }

    private static Il2CppFMOD.System GetSystem(IntPtr sound) {
        m_getSystemObject ??= ResolveExport<SoundGetSystemObjectDelegate>("FMOD5_Sound_GetSystemObject");
        int status = m_getSystemObject(sound, out IntPtr handle);
        if (status != 0 || handle == IntPtr.Zero)
            throw new InvalidOperationException($"FMOD Sound::getSystemObject returned {status}");

        return new Il2CppFMOD.System { handle = handle };
    }

    private static T ResolveExport<T>(string exportName) where T : Delegate {
        IntPtr module = GetModuleHandle("fmodstudioL.dll");
        if (module == IntPtr.Zero) module = GetModuleHandle("fmodstudio.dll");
        if (module == IntPtr.Zero) throw new InvalidOperationException("FMOD runtime module is not loaded");

        IntPtr address = GetProcAddress(module, exportName);
        if (address == IntPtr.Zero) throw new InvalidOperationException(exportName + " export is unavailable");

        return Marshal.GetDelegateForFunctionPointer<T>(address);
    }

    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetModuleHandle(string moduleName);

    [DllImport("kernel32", CharSet = CharSet.Ansi, SetLastError = true)]
    private static extern IntPtr GetProcAddress(IntPtr module, string procedureName);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SoundGetSystemObjectDelegate(IntPtr sound, out IntPtr system);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelGetCurrentSoundDelegate(IntPtr channel, out IntPtr sound);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelStopDelegate(IntPtr channel);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int ChannelIsPlayingDelegate(IntPtr channel, out int playing);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SoundGetOpenStateDelegate(IntPtr sound, out OPENSTATE state, out uint percentBuffered, out int starving, out int diskBusy);

    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int SoundReleaseDelegate(IntPtr sound);

    private sealed class PendingController {
        internal PendingController(_Cg controller, RouteState route) {
            Controller = controller;
            Route = route;
        }

        internal _Cg Controller { get; }
        internal RouteState Route { get; }
        internal Stopwatch Elapsed { get; } = Stopwatch.StartNew();
        internal OwnedSound Sound { get; set; }
        internal IntPtr OriginalChannel { get; set; }
        internal bool SeekIssued { get; set; }
    }

    private sealed class OwnedSound {
        internal OwnedSound(_Cg controller, RouteState route, IntPtr handle) {
            Controller = controller;
            Route = route;
            Handle = handle;
        }

        internal _Cg Controller { get; }
        internal RouteState Route { get; }
        internal IntPtr Handle { get; }
        internal IntPtr Channel { get; set; }
    }

    private sealed class RouteState {
        internal CustomSongConfig Config;
        internal Il2CppSystem.Action<_Cg> SelectorCallback;
        internal Il2CppSystem.Action<_Cg> FactoryCallback;
        internal long Generation;
        internal bool IsSelection;
        internal bool NativeReady;
    }
}
