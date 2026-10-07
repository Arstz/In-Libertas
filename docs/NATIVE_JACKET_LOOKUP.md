# Native jacket method lookup

Hook 0.1.2 resolves `AddressablesImpl.LoadAssetAsync<Material>(object)` through
IL2CPP metadata at startup. It does not use a GameAssembly RVA, file hash,
byte-pattern search, method token, or generated interop field name to locate
the function.

The lookup requires a unique instance generic definition with one exact
`System.Object` parameter, excluding the `IResourceLocation` overload. Native
reflection closes that definition over `Material`. The resulting method must
have the expected declaring class, object parameter, and exact
`AsyncOperationHandle<Material>` return class. The entry pointer is read with
Il2CppInterop's Unity-version-specific MethodInfo accessor.

The non-generic `AsyncOperationHandle.Acquire()` lookup also requires an exact,
unique instance signature. Both handle types must have the expected x64 value
size and alignment before either pointer is handed to the companion DLL.

The companion still checks its API version and the complete known 16-byte
prologue before installing the tested trampoline. Address changes alone no
longer require a rebuild. An unfamiliar prologue, missing or ambiguous method,
or incompatible handle layout disables custom jacket interception and retains
the template references. Byte scanning is not attempted as a fallback.

This is method discovery, not a general instruction-relocating detour engine.
A compiler/prologue or Unity ABI change can still require a bridge update.

Build using `tools/build.ps1 -Configuration Release` and redeploy both DLLs
from `build/Mods`. `tools/verify_hook_lookup.ps1` checks the corresponding
signatures in the installed Cpp2IL output without starting the game. The
native bridge regression test separately checks its trampoline and marker
ownership behavior; runtime lookup and jacket rendering still need an in-game
test after redeployment.
