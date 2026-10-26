using System;
using System.Runtime.InteropServices;

namespace InFalsusCustomSongHook;

internal static class ChartEventBufferPatch
{
    private const int kEventCapacity = 100000;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int InstallOverride(IntPtr constructor, int capacity);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int RemoveOverride();
    private static bool _installed;

    internal static void Install()
    {
        if (_installed) return;
        try {
            IntPtr target = NativeMethodLookup.ResolveChartStateConstructor();
            InstallOverride install = NativeRuntimeOverrideBridge.Resolve<InstallOverride>("InstallChartEventCapacityOverride");
            if (install(target, kEventCapacity) == 0)
                throw new InvalidOperationException("The native chart event allocation code does not match the validated build.");
            _installed = true;
            CustomSongMod.Log.Msg("[CustomSong] native chart event allocation capacities: records 1024->" + kEventCapacity
                + ", coordinates 8096->" + kEventCapacity);
        }
        catch (Exception exception) {
            CustomSongMod.Log.Error("[CustomSong] chart event buffer hook installation failed: " + exception);
        }
    }

    internal static void Dispose()
    {
        if (!_installed) return;
        RemoveOverride remove = NativeRuntimeOverrideBridge.Resolve<RemoveOverride>("RemoveChartEventCapacityOverride");
        if (remove() == 0) throw new InvalidOperationException("The native chart event allocation override could not be removed.");
        _installed = false;
    }
}
