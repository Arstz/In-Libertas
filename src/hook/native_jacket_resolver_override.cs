using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Reflection;
using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.InteropTypes;
using UnityEngine;
using UnityEngine.AddressableAssets;

namespace InFalsusCustomSongHook;

/// <summary>
/// An independent x64 detour for the deployed private jacket-operation
/// builder.  It deliberately does not use MelonLoader's detour/trampoline or
/// an injected ResourceManager provider.
/// </summary>
internal static class NativeJacketResolverOverride
{
    // GameAssembly.deployed.20261003.dll: sub_180020370. Its only callers are
    // the SongId and ChartId jacket resolver helpers.
    private static readonly IntPtr SharedJacketBuilderRva = new(0x20370);
    private static IntPtr _bridgeModule;
    private static bool _installed;
    private static readonly object RegistrationGate = new();
    private static readonly Dictionary<string, RegisteredJacketPair> RegisteredPairs = new(StringComparer.Ordinal);

    [StructLayout(LayoutKind.Sequential)]
    private struct NativeAsyncOperationHandle
    {
        internal IntPtr InternalOperation;
        internal int Version;
        internal int Padding;
        internal IntPtr LocationName;
    }

    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int InstallBridgeDelegate(IntPtr target);
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate void RemoveBridgeDelegate();
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate int GetBridgeCallCountDelegate();
    [UnmanagedFunctionPointer(CallingConvention.Winapi)]
    private delegate void RegisterReplacementDelegate(
        IntPtr largeMarker, IntPtr smallMarker,
        ref NativeAsyncOperationHandle largeHandle, ref NativeAsyncOperationHandle smallHandle);

    internal static void Install()
    {
        if (_installed) return;
        try
        {
            IntPtr target = new(GetGameAssemblyBase().ToInt64() + SharedJacketBuilderRva.ToInt64());
            IntPtr bridge = LoadBridge();
            InstallBridgeDelegate install = Marshal.GetDelegateForFunctionPointer<InstallBridgeDelegate>(
                NativeLibrary.GetExport(bridge, "InstallJacketBuilderBridge"));
            if (install(target) == 0)
                throw new InvalidOperationException("The native bridge could not install its jacket-builder trampoline.");
            _installed = true;
            GetBridgeCallCountDelegate count = Marshal.GetDelegateForFunctionPointer<GetBridgeCallCountDelegate>(
                NativeLibrary.GetExport(bridge, "GetJacketBuilderBridgeCallCount"));
            CustomSongMod.Log.Msg("[CustomSong] installed independent native jacket resolver bridge; target=0x" +
                                  target.ToInt64().ToString("X") + "; initialBridgeCalls=" + count() + ".");
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Warning("[CustomSong] native jacket resolver bridge was not installed: " + exception.Message);
        }
    }

    internal static bool TryConfigure(Material largeMaterial, Material smallMaterial,
        out AssetReferenceT<Material> largeMarker, out AssetReferenceT<Material> smallMarker)
    {
        largeMarker = null;
        smallMarker = null;
        if (!_installed || largeMaterial is null || smallMaterial is null) return false;
        try
        {
            string identity = largeMaterial.Pointer.ToInt64().ToString("X") + ":" + smallMaterial.Pointer.ToInt64().ToString("X");
            lock (RegistrationGate)
            {
                if (!RegisteredPairs.TryGetValue(identity, out RegisteredJacketPair pair))
                {
                    object largeHandleRoot = CreateCompletedMaterialHandle(largeMaterial, out NativeAsyncOperationHandle largeHandle);
                    object smallHandleRoot = CreateCompletedMaterialHandle(smallMaterial, out NativeAsyncOperationHandle smallHandle);
                    AssetReferenceT<Material> nextLargeMarker = new(Guid.NewGuid().ToString("N"));
                    AssetReferenceT<Material> nextSmallMarker = new(Guid.NewGuid().ToString("N"));

                    RegisterReplacementDelegate register = Marshal.GetDelegateForFunctionPointer<RegisterReplacementDelegate>(
                        NativeLibrary.GetExport(LoadBridge(), "RegisterJacketReplacement"));
                    register(GetNativeObjectPointer(nextLargeMarker), GetNativeObjectPointer(nextSmallMarker), ref largeHandle, ref smallHandle);
                    pair = new RegisteredJacketPair(nextLargeMarker, nextSmallMarker, largeHandleRoot, smallHandleRoot);
                    RegisteredPairs.Add(identity, pair);
                    CustomSongMod.Log.Msg("[CustomSong] registered native completed-material jacket pair " + RegisteredPairs.Count + ".");
                }
                largeMarker = pair.LargeMarker;
                smallMarker = pair.SmallMarker;
            }
            return true;
        }
        catch (TargetInvocationException exception) when (exception.InnerException is not null)
        {
            CustomSongMod.Log.Warning("[CustomSong] native completed-material jacket setup failed: " + exception.InnerException.Message);
            return false;
        }
        catch (Exception exception)
        {
            CustomSongMod.Log.Warning("[CustomSong] native completed-material jacket setup failed: " + exception.Message);
            return false;
        }
    }

    internal static void Dispose()
    {
        if (_bridgeModule == IntPtr.Zero) return;
        try
        {
            RemoveBridgeDelegate remove = Marshal.GetDelegateForFunctionPointer<RemoveBridgeDelegate>(
                NativeLibrary.GetExport(_bridgeModule, "RemoveJacketBuilderBridge"));
            remove();
        }
        catch { }
        finally
        {
            _installed = false;
            lock (RegistrationGate) RegisteredPairs.Clear();
            NativeLibrary.Free(_bridgeModule);
            _bridgeModule = IntPtr.Zero;
        }
    }

    private static object CreateCompletedMaterialHandle(Material material, out NativeAsyncOperationHandle nativeHandle)
    {
        object resourceManager = Addressables.ResourceManager
            ?? throw new InvalidOperationException("Addressables.ResourceManager is unavailable.");
        MethodInfo factory = null;
        foreach (MethodInfo candidate in resourceManager.GetType().GetMethods(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic))
        {
            if (candidate.Name == "CreateCompletedOperation" && candidate.IsGenericMethodDefinition && candidate.GetParameters().Length == 2)
            {
                factory = candidate.MakeGenericMethod(typeof(Material));
                break;
            }
        }
        if (factory is null) throw new MissingMethodException(resourceManager.GetType().FullName, "CreateCompletedOperation<T>(T,string)");

        object boxedHandle = factory.Invoke(resourceManager, new object[] { material, null })
            ?? throw new InvalidOperationException("CreateCompletedOperation<Material> returned null.");
        if (boxedHandle is not Il2CppObjectBase native || native.Pointer == IntPtr.Zero)
            throw new InvalidOperationException("CreateCompletedOperation<Material> did not return an IL2CPP boxed handle.");
        IntPtr unboxed = IL2CPP.il2cpp_object_unbox(native.Pointer);
        if (unboxed == IntPtr.Zero) throw new InvalidOperationException("The completed Material handle could not be unboxed.");

        nativeHandle = new NativeAsyncOperationHandle
        {
            InternalOperation = Marshal.ReadIntPtr(unboxed),
            Version = Marshal.ReadInt32(unboxed, IntPtr.Size),
            Padding = Marshal.ReadInt32(unboxed, IntPtr.Size + sizeof(int)),
            LocationName = Marshal.ReadIntPtr(unboxed, IntPtr.Size + (sizeof(int) * 2)),
        };
        if (nativeHandle.InternalOperation == IntPtr.Zero)
            throw new InvalidOperationException("The completed Material handle has no internal operation.");
        return boxedHandle;
    }

    private static IntPtr GetNativeObjectPointer(object value)
    {
        if (value is Il2CppObjectBase native && native.Pointer != IntPtr.Zero) return native.Pointer;
        for (Type type = value?.GetType(); type is not null; type = type.BaseType)
        {
            foreach (PropertyInfo property in type.GetProperties(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
            {
                if (property.PropertyType != typeof(IntPtr) || property.GetIndexParameters().Length != 0) continue;
                if (property.Name is not "Pointer" and not "ptr" and not "m_Ptr" and not "m_CachedPtr") continue;
                if (property.GetValue(value) is IntPtr pointer && pointer != IntPtr.Zero) return pointer;
            }
            foreach (FieldInfo field in type.GetFields(BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic | BindingFlags.DeclaredOnly))
            {
                if (field.FieldType != typeof(IntPtr)) continue;
                if (field.Name is not "Pointer" and not "ptr" and not "m_Ptr" and not "m_CachedPtr") continue;
                if (field.GetValue(value) is IntPtr pointer && pointer != IntPtr.Zero) return pointer;
            }
        }
        throw new InvalidOperationException("The marker AssetReference has no discoverable IL2CPP object pointer.");
    }

    private static IntPtr LoadBridge()
    {
        if (_bridgeModule != IntPtr.Zero) return _bridgeModule;
        string directory = Path.GetDirectoryName(Assembly.GetExecutingAssembly().Location)
            ?? throw new InvalidOperationException("The hook assembly has no directory.");
        string path = Path.Combine(directory, "InFalsusNativeJacketResolverBridge.dll");
        if (!File.Exists(path)) throw new FileNotFoundException("Native jacket resolver bridge is missing.", path);
        _bridgeModule = NativeLibrary.Load(path);
        return _bridgeModule;
    }

    private static IntPtr GetGameAssemblyBase()
    {
        foreach (ProcessModule module in Process.GetCurrentProcess().Modules)
            if (string.Equals(module.ModuleName, "GameAssembly.dll", StringComparison.OrdinalIgnoreCase))
                return module.BaseAddress;
        throw new InvalidOperationException("GameAssembly.dll is not loaded.");
    }

    private sealed class RegisteredJacketPair
    {
        internal RegisteredJacketPair(
            AssetReferenceT<Material> largeMarker,
            AssetReferenceT<Material> smallMarker,
            object largeHandleRoot,
            object smallHandleRoot)
        {
            LargeMarker = largeMarker;
            SmallMarker = smallMarker;
            LargeHandleRoot = largeHandleRoot;
            SmallHandleRoot = smallHandleRoot;
        }

        internal AssetReferenceT<Material> LargeMarker { get; }
        internal AssetReferenceT<Material> SmallMarker { get; }
        // Root the boxed IL2CPP handles while their InternalOp values are
        // reachable from the native marker map.
        private object LargeHandleRoot { get; }
        private object SmallHandleRoot { get; }
    }
}
