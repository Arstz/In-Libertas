#if BEPINEX
using System;
using BepInEx;
using BepInEx.Unity.IL2CPP;
using HarmonyLib;
using UnityEngine;

namespace InFalsusCustomSongHook;

[BepInPlugin("Arstz.InLibertas.CustomSongs", "In Falsus Custom Song Hook", CustomSongRuntime.Version)]
[BepInProcess("infalsus.exe")]
public sealed class CustomSongPlugin : BasePlugin {
    private CustomSongTick m_tick;

    public override void Load() {
        CustomSongRuntime.Initialize(Paths.GameRootPath,
            new HookLogger(message => Log.LogInfo(message), message => Log.LogWarning(message),
                message => Log.LogError(message)), new HarmonyLib.Harmony("Arstz.InLibertas.CustomSongs"));
        m_tick = AddComponent<CustomSongTick>();
    }

    public override bool Unload() {
        if (m_tick != null) UnityEngine.Object.Destroy(m_tick);
        CustomSongRuntime.Shutdown();
        return true;
    }
}

public sealed class CustomSongTick : MonoBehaviour {
    public CustomSongTick(IntPtr pointer) : base(pointer) { }
    private void Update() => CustomSongRuntime.Update();
}
#endif
