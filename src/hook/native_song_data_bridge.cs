using System;
using Il2CppInterop.Runtime;
using Il2Cppifapp.Game.Data;

namespace InFalsusCustomSongHook;

/// <summary>
/// Build-pinned SongData jacket-cache reset. GameAssembly's _yOA uses field
/// a1[9] (object offset 0x48) for SongId references and a1[10] (object offset
/// 0x50) for ChartId references. A null field is reconstructed from its
/// corresponding serialized jacket-entry array.
/// </summary>
internal static class NativeSongDataBridge
{
    private const int kSongIdJacketCacheOffset = 0x48;
    private const int kChartIdJacketCacheOffset = 0x50;

    internal static void ResetSongIdJacketCache(SongData songData)
    {
        IntPtr instance = songData.Pointer;
        if (instance == IntPtr.Zero) throw new InvalidOperationException("SongData has no native object.");
        IntPtr field = IntPtr.Add(instance, kSongIdJacketCacheOffset);
        IL2CPP.il2cpp_gc_wbarrier_set_field(instance, field, IntPtr.Zero);
    }

    internal static void ResetChartIdJacketCache(SongData songData)
    {
        IntPtr instance = songData.Pointer;
        if (instance == IntPtr.Zero) throw new InvalidOperationException("SongData has no native object.");
        IntPtr field = IntPtr.Add(instance, kChartIdJacketCacheOffset);
        IL2CPP.il2cpp_gc_wbarrier_set_field(instance, field, IntPtr.Zero);
    }
}
