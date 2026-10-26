using System;
using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using Il2Cppifapp.Game.Data;

namespace InFalsusCustomSongHook;

internal static class CustomSongTitleLocalizationPatch
{
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int InstallResolver(IntPtr target);
    [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
    private delegate int RemoveResolver();
    private static bool _installed;

    internal static void Install()
    {
        if (_installed) return;
        try {
            IntPtr owner = Il2CppClassPointerStore<DynamicStringMapping.TextMappingValues>.NativeClassPtr;
            uint alignment = 0;
            int valueSize = IL2CPP.il2cpp_class_value_size(owner, ref alignment);
            if (IntPtr.Size != 8 || !IL2CPP.il2cpp_class_is_valuetype(owner)
                || valueSize != 40 || alignment != IntPtr.Size)
                throw new InvalidOperationException("The native localized-string layout is incompatible.");
            IntPtr field = IL2CPP.GetIl2CppField(owner, nameof(DynamicStringMapping.TextMappingValues.Korean));
            int koreanOffset = checked((int)IL2CPP.il2cpp_field_get_offset(field)) - 2 * IntPtr.Size;
            if (koreanOffset != 2 * IntPtr.Size)
                throw new InvalidOperationException("The native Korean-string field offset is incompatible.");

            IntPtr target = NativeMethodLookup.ResolveLocalizedStringResolver();
            InstallResolver install = NativeRuntimeOverrideBridge.Resolve<InstallResolver>("InstallLocalizedStringResolver");
            if (install(target) == 0)
                throw new InvalidOperationException("The native localized-string resolver does not match the validated build.");
            _installed = true;
            CustomSongMod.Log.Msg("[CustomSong] installed compiled native localized-string resolver.");
        }
        catch (Exception exception) {
            CustomSongMod.Log.Error("[CustomSong] localized-string resolver hook installation failed: " + exception);
        }
    }

    internal static void Dispose()
    {
        if (!_installed) return;
        RemoveResolver remove = NativeRuntimeOverrideBridge.Resolve<RemoveResolver>("RemoveLocalizedStringResolver");
        if (remove() == 0) throw new InvalidOperationException("The native localized-string resolver could not be removed.");
        _installed = false;
    }
}
