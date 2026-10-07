using System;
using System.Reflection;
using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.Runtime;
using Il2CppInterop.Runtime.InteropTypes.Arrays;
using UnityEngine;
using UnityEngine.ResourceManagement.AsyncOperations;

namespace InFalsusCustomSongHook;

internal static class NativeMethodLookup {
    internal static IntPtr ResolveMaterialLoader(int expectedHandleSize) {
        IntPtr owner = RequireClass("Unity.Addressables.dll", "UnityEngine.AddressableAssets", "AddressablesImpl");
        IntPtr objectClass = RequireClass("mscorlib.dll", "System", "Object");
        IntPtr definition = FindUniqueInstanceMethod(owner, "LoadAssetAsync", true, objectClass);
        var reflectedDefinition = ReflectMethod(definition, owner);

        if (reflectedDefinition.GetGenericArguments().Length != 1)
            throw new InvalidOperationException("AddressablesImpl.LoadAssetAsync must have exactly one type parameter.");
        var arguments = new Il2CppReferenceArray<Il2CppSystem.Type>(new[] { Il2CppType.Of<Material>() });
        var closedMethod = reflectedDefinition.MakeGenericMethod(arguments);
        if (closedMethod.ContainsGenericParameters || closedMethod.IsStatic)
            throw new InvalidOperationException("AddressablesImpl.LoadAssetAsync<Material> is not a closed instance method.");
        IntPtr methodInfo = IL2CPP.il2cpp_method_get_from_reflection(closedMethod.Pointer);
        IntPtr handleClass = Il2CppClassPointerStore<AsyncOperationHandle<Material>>.NativeClassPtr;
        RequireHandleLayout(handleClass, expectedHandleSize);
        RequireSignature(methodInfo, owner, handleClass, objectClass);

        return GetEntryPoint(methodInfo);
    }

    internal static IntPtr ResolveHandleAcquire(int expectedHandleSize) {
        IntPtr owner = RequireClass(
            "Unity.ResourceManager.dll", "UnityEngine.ResourceManagement.AsyncOperations", "AsyncOperationHandle");
        IntPtr methodInfo = FindUniqueInstanceMethod(owner, "Acquire", false);

        RequireHandleLayout(owner, expectedHandleSize);
        RequireSignature(methodInfo, owner, owner);

        return methodInfo;
    }

    private static IntPtr RequireClass(string assembly, string nameSpace, string name) {
        IntPtr nativeClass = IL2CPP.GetIl2CppClass(assembly, nameSpace, name);

        if (nativeClass == IntPtr.Zero)
            throw new TypeLoadException($"Native class {nameSpace}.{name} is unavailable in {assembly}.");

        return nativeClass;
    }

    private static IntPtr FindUniqueInstanceMethod(
        IntPtr owner, string name, bool genericDefinition, params IntPtr[] parameterClasses) {
        IntPtr iterator = IntPtr.Zero;
        IntPtr match = IntPtr.Zero;

        while (true) {
            IntPtr candidate = IL2CPP.il2cpp_class_get_methods(owner, ref iterator);
            if (candidate == IntPtr.Zero) break;
            if (!string.Equals(Marshal.PtrToStringAnsi(IL2CPP.il2cpp_method_get_name(candidate)), name, StringComparison.Ordinal))
                continue;
            if (!HasParameters(candidate, parameterClasses)) continue;
            var reflected = ReflectMethod(candidate, owner);
            if (reflected.IsStatic || reflected.IsGenericMethodDefinition != genericDefinition ||
                reflected.IsGenericMethod != genericDefinition)
                continue;
            if (match != IntPtr.Zero)
                throw new AmbiguousMatchException($"Native instance method {name} has multiple matching overloads.");
            match = candidate;
        }
        if (match == IntPtr.Zero)
            throw new MissingMethodException($"Native instance method {name} has no matching signature.");

        return match;
    }

    private static bool HasParameters(IntPtr methodInfo, IntPtr[] parameterClasses) {
        if (IL2CPP.il2cpp_method_get_param_count(methodInfo) != parameterClasses.Length) return false;
        for (int index = 0; index < parameterClasses.Length; index++) {
            IntPtr actualType = IL2CPP.il2cpp_method_get_param(methodInfo, (uint)index);
            IntPtr expectedType = IL2CPP.il2cpp_class_get_type(parameterClasses[index]);
            if (actualType == IntPtr.Zero || IL2CPP.il2cpp_type_is_byref(actualType) ||
                !IL2CPP.il2cpp_type_equals(actualType, expectedType))
                return false;
        }

        return true;
    }

    private static Il2CppSystem.Reflection.MethodInfo ReflectMethod(IntPtr methodInfo, IntPtr owner) {
        IntPtr reflection = IL2CPP.il2cpp_method_get_object(methodInfo, owner);

        if (reflection == IntPtr.Zero)
            throw new InvalidOperationException("The native method could not be reflected.");

        return new Il2CppSystem.Reflection.MethodInfo(reflection);
    }

    private static void RequireSignature(
        IntPtr methodInfo, IntPtr owner, IntPtr returnClass, params IntPtr[] parameterClasses) {
        if (methodInfo == IntPtr.Zero || IL2CPP.il2cpp_method_get_class(methodInfo) != owner ||
            !HasParameters(methodInfo, parameterClasses))
            throw new InvalidOperationException("The resolved native method has an incompatible owner or parameter signature.");
        IntPtr returnType = IL2CPP.il2cpp_method_get_return_type(methodInfo);
        if (returnType == IntPtr.Zero || IL2CPP.il2cpp_type_is_byref(returnType) ||
            IL2CPP.il2cpp_class_from_type(returnType) != returnClass)
            throw new InvalidOperationException("The resolved native method has an incompatible return type.");
    }

    private static void RequireHandleLayout(IntPtr handleClass, int expectedSize) {
        uint alignment = 0;

        if (IntPtr.Size != sizeof(long) || handleClass == IntPtr.Zero ||
            !IL2CPP.il2cpp_class_is_valuetype(handleClass) ||
            IL2CPP.il2cpp_class_value_size(handleClass, ref alignment) != expectedSize ||
            alignment != IntPtr.Size)
            throw new InvalidOperationException("The native operation-handle layout is incompatible with the x64 bridge.");
    }

    internal static unsafe IntPtr GetEntryPoint(IntPtr methodInfo) {
        if (methodInfo == IntPtr.Zero)
            throw new InvalidOperationException("The native method metadata is unavailable.");
        IntPtr entry = UnityVersionHandler.Wrap((Il2CppMethodInfo*)methodInfo).MethodPointer;
        if (entry == IntPtr.Zero)
            throw new InvalidOperationException("The resolved native method has no entry point.");

        return entry;
    }
}
