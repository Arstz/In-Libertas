using System;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;

namespace InFalsusCustomSongHook;

internal static class NativeRuntimeOverrideBridge
{
    private static IntPtr _module;

    internal static T Resolve<T>(string name) where T : Delegate
    {
        if (_module == IntPtr.Zero) {
            string directory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location)
                ?? throw new InvalidOperationException("The hook assembly has no directory.");
            _module = NativeLibrary.Load(Path.Combine(directory, "InFalsusNativeJacketResolverBridge.dll"));
        }
        return Marshal.GetDelegateForFunctionPointer<T>(NativeLibrary.GetExport(_module, name));
    }

    internal static void Dispose()
    {
        if (_module == IntPtr.Zero) return;
        NativeLibrary.Free(_module);
        _module = IntPtr.Zero;
    }
}
