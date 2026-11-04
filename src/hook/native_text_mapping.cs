#if BEPINEX
using System;
using System.Runtime.InteropServices;
using Il2CppInterop.Runtime;
using Il2CppInterop.Runtime.InteropTypes;
using ifapp.Game.Data;

namespace InFalsusCustomSongHook;

internal static unsafe class NativeTextMapping {
    internal static void StoreValue(
        Il2CppSystem.Collections.Generic.List<DynamicStringMapping.TextMappingValues> list,
        DynamicStringMapping.TextMappingValues values,
        int? index = null) {
        IntPtr listClass = IL2CPP.il2cpp_object_get_class(list.Pointer);
        IntPtr method = IL2CPP.il2cpp_class_get_method_from_name(listClass,
            index.HasValue ? "set_Item" : "Add", index.HasValue ? 2 : 1);
        if (method == IntPtr.Zero) throw new MissingMethodException("The native text list writer is unavailable.");
        uint parameter = index.HasValue ? 1u : 0u;
        IntPtr valueClass = IL2CPP.il2cpp_class_from_type(IL2CPP.il2cpp_method_get_param(method, parameter));
        uint alignment = 0;
        if (!IL2CPP.il2cpp_class_is_valuetype(valueClass)
            || IL2CPP.il2cpp_class_value_size(valueClass, ref alignment) != 5 * IntPtr.Size
            || IL2CPP.il2cpp_object_get_class(values.Pointer) != valueClass)
            throw new InvalidOperationException("The native text list value layout is incompatible.");
        int position = index.GetValueOrDefault();
        void** arguments = stackalloc void*[2];
        arguments[0] = &position;
        arguments[parameter] = (void*)IL2CPP.il2cpp_object_unbox(values.Pointer);
        IntPtr exception = IntPtr.Zero;
        IL2CPP.il2cpp_runtime_invoke(method, list.Pointer, arguments, ref exception);
        Il2CppException.RaiseExceptionIfNecessary(exception);
        GC.KeepAlive(values);
        GC.KeepAlive(list);
    }

    internal static void Set<T>(Il2CppObjectBase mapping, T id, Il2CppObjectBase values) {
        ushort key = id switch {
            ifapp.Game.Data.SongId song => song.Value,
            ifapp.Game.Data.PackId pack => pack.Value,
            _ => throw new InvalidOperationException("The native text mapping key type is unsupported."),
        };
        IntPtr owner = IL2CPP.il2cpp_object_get_class(mapping.Pointer);
        IntPtr field = IL2CPP.il2cpp_class_get_field_from_name(owner, "Mapping");
        if (field == IntPtr.Zero) throw new MissingFieldException("The native text dictionary is unavailable.");
        int offset = checked((int)IL2CPP.il2cpp_field_get_offset(field));
        if (offset < 2 * IntPtr.Size || offset + IntPtr.Size > IL2CPP.il2cpp_class_instance_size(owner))
            throw new InvalidOperationException("The native text dictionary field layout is incompatible.");
        IntPtr dictionary = Marshal.ReadIntPtr(mapping.Pointer, offset);
        if (dictionary == IntPtr.Zero) throw new InvalidOperationException("The native text dictionary is uninitialized.");
        IntPtr dictionaryClass = IL2CPP.il2cpp_object_get_class(dictionary);
        IntPtr setter = IL2CPP.il2cpp_class_get_method_from_name(dictionaryClass, "set_Item", 2);
        if (setter == IntPtr.Zero || IL2CPP.il2cpp_method_get_param_count(setter) != 2)
            throw new MissingMethodException("The native text dictionary setter is unavailable.");
        IntPtr keyClass = IL2CPP.il2cpp_class_from_type(IL2CPP.il2cpp_method_get_param(setter, 0));
        IntPtr valueClass = IL2CPP.il2cpp_class_from_type(IL2CPP.il2cpp_method_get_param(setter, 1));
        uint alignment = 0;
        if (!IL2CPP.il2cpp_class_is_valuetype(keyClass) || IL2CPP.il2cpp_class_value_size(keyClass, ref alignment) != sizeof(ushort)
            || !IL2CPP.il2cpp_class_is_valuetype(valueClass) || IL2CPP.il2cpp_class_value_size(valueClass, ref alignment) != 5 * IntPtr.Size
            || IL2CPP.il2cpp_object_get_class(values.Pointer) != valueClass)
            throw new InvalidOperationException("The native text dictionary signature is incompatible.");
        void** arguments = stackalloc void*[2];
        arguments[0] = &key;
        arguments[1] = (void*)IL2CPP.il2cpp_object_unbox(values.Pointer);
        IntPtr exception = IntPtr.Zero;
        IL2CPP.il2cpp_runtime_invoke(setter, dictionary, arguments, ref exception);
        Il2CppException.RaiseExceptionIfNecessary(exception);
        GC.KeepAlive(mapping);
        GC.KeepAlive(values);
    }
}
#endif
