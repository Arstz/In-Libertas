using System;
using System.Runtime.InteropServices;

namespace InFalsusCustomSongHook;

internal static class ChartEventBufferPatch
{
    private const int kEventCapacity = 100000;
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int InstallOverride(IntPtr constructor, int capacity, int recordFieldOffset, int coordinateFieldOffset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int QueryOverride(out int originalRecords, out int originalCoordinates,
        out int recordInstructionOffset, out int coordinateInstructionOffset);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int RemoveOverride();
    private static bool _installed;

    internal static void Install()
    {
        if (_installed) return;
        try {
            IntPtr target = NativeMethodLookup.ResolveChartStateConstructor(out int recordFieldOffset, out int coordinateFieldOffset);
            InstallOverride install = NativeRuntimeOverrideBridge.Resolve<InstallOverride>("InstallChartEventCapacityOverrideForFields");
            if (install(target, kEventCapacity, recordFieldOffset, coordinateFieldOffset) == 0)
                throw new InvalidOperationException("The native chart state constructor has no unique compatible event allocation pair.");
            _installed = true;
            QueryOverride query = NativeRuntimeOverrideBridge.Resolve<QueryOverride>("GetChartEventCapacityOverrideInfo");
            if (query(out int originalRecords, out int originalCoordinates, out int recordInstructionOffset,
                    out int coordinateInstructionOffset) == 0)
                throw new InvalidOperationException("The installed native chart event allocation override could not be queried.");
            CustomSongMod.Log.Msg("[CustomSong] native chart event allocation capacities: records " + originalRecords + "->" + kEventCapacity
                + ", coordinates " + originalCoordinates + "->" + kEventCapacity
                + "; scanned constructor offsets +0x" + recordInstructionOffset.ToString("X")
                + ", +0x" + coordinateInstructionOffset.ToString("X"));
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
