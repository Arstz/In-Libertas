// Marker substitution precedes stock operation-owner registration.

#include <windows.h>
#include <cstring>
#include <unordered_map>

using JacketBuilder = void* (__fastcall*)(void* outputHandle,
                                          void* resolverPipeline,
                                          void* assetReference,
                                          void* methodInfo);

struct AsyncOperationHandle {
    void* internalOperation;
    int version;
    int padding;
    void* locationName;
};

using AcquireHandle = AsyncOperationHandle* (__fastcall*)(AsyncOperationHandle* outputHandle,
                                                         const AsyncOperationHandle* sourceHandle,
                                                         void* methodInfo);
static constexpr BYTE kPipelinePrologue[] = {
    0x48, 0x8B, 0xC4, 0x4C, 0x89, 0x48, 0x20, 0x48,
    0x89, 0x50, 0x10, 0x48, 0x89, 0x48, 0x08, 0x53
};
static_assert(sizeof(AsyncOperationHandle) == 24);
static constexpr int kBridgeApiVersion = 1;

static JacketBuilder g_originalBuilder = nullptr;
static AcquireHandle g_acquireHandle = nullptr;
static void* g_acquireMethodInfo = nullptr;
static volatile LONG g_callCount = 0;
static void* g_target = nullptr;
static BYTE g_originalBytes[sizeof(kPipelinePrologue)]{};
static void* g_trampoline = nullptr;
static SRWLOCK g_replacementsLock = SRWLOCK_INIT;
static std::unordered_map<void*, AsyncOperationHandle> g_markerHandles;

extern "C" __declspec(dllexport) void* __fastcall JacketBuilderBridge(
    void* outputHandle, void* resolverPipeline, void* assetReference, void* methodInfo);

static void writeAbsoluteJump(BYTE* destination, const void* target) {
    destination[0] = 0xFF;
    destination[1] = 0x25;
    *reinterpret_cast<DWORD*>(destination + 2) = 0;
    *reinterpret_cast<const void**>(destination + 6) = target;
}

extern "C" __declspec(dllexport) BOOL __fastcall InstallJacketBuilderBridge(
    void* target, AcquireHandle acquireHandle, void* acquireMethodInfo) {
    if (g_target != nullptr) return g_target == target;
    if (target == nullptr || acquireHandle == nullptr || acquireMethodInfo == nullptr) return FALSE;
    BYTE* source = static_cast<BYTE*>(target);
    if (memcmp(source, kPipelinePrologue, sizeof(kPipelinePrologue)) != 0) return FALSE;

    constexpr SIZE_T kTrampolineSize = sizeof(kPipelinePrologue) + 14;
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr, kTrampolineSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (trampoline == nullptr) return FALSE;

    memcpy(g_originalBytes, source, sizeof(g_originalBytes));
    memcpy(trampoline, g_originalBytes, sizeof(g_originalBytes));
    writeAbsoluteJump(trampoline + sizeof(g_originalBytes), source + sizeof(g_originalBytes));

    DWORD oldProtection = 0;
    if (!VirtualProtect(source, sizeof(g_originalBytes), PAGE_EXECUTE_READWRITE, &oldProtection)) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return FALSE;
    }
    g_acquireHandle = acquireHandle;
    g_acquireMethodInfo = acquireMethodInfo;
    g_originalBuilder = reinterpret_cast<JacketBuilder>(trampoline);
    FlushInstructionCache(GetCurrentProcess(), trampoline, kTrampolineSize);
    writeAbsoluteJump(source, reinterpret_cast<void*>(&JacketBuilderBridge));
    source[14] = 0x90;
    source[15] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(source, sizeof(g_originalBytes), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), source, sizeof(g_originalBytes));

    g_target = target;
    g_trampoline = trampoline;

    return TRUE;
}

extern "C" __declspec(dllexport) void __fastcall RegisterJacketReplacement(
    void* largeMarker, void* smallMarker,
    const AsyncOperationHandle* largeHandle, const AsyncOperationHandle* smallHandle) {
    AcquireSRWLockExclusive(&g_replacementsLock);
    if (largeMarker != nullptr && largeHandle != nullptr)
        g_markerHandles[largeMarker] = *largeHandle;
    if (smallMarker != nullptr && smallHandle != nullptr)
        g_markerHandles[smallMarker] = *smallHandle;
    ReleaseSRWLockExclusive(&g_replacementsLock);
}

extern "C" __declspec(dllexport) LONG __fastcall GetJacketBuilderBridgeCallCount() {

    return g_callCount;
}

extern "C" __declspec(dllexport) int __fastcall GetJacketBuilderBridgeApiVersion() {

    return kBridgeApiVersion;
}

extern "C" __declspec(dllexport) void __fastcall RemoveJacketBuilderBridge() {
    if (g_target == nullptr) return;
    BYTE* source = static_cast<BYTE*>(g_target);
    DWORD oldProtection = 0;
    if (VirtualProtect(source, sizeof(g_originalBytes), PAGE_EXECUTE_READWRITE, &oldProtection)) {
        memcpy(source, g_originalBytes, sizeof(g_originalBytes));
        DWORD ignored = 0;
        VirtualProtect(source, sizeof(g_originalBytes), oldProtection, &ignored);
        FlushInstructionCache(GetCurrentProcess(), source, sizeof(g_originalBytes));
    }
    if (g_trampoline != nullptr) VirtualFree(g_trampoline, 0, MEM_RELEASE);
    g_target = nullptr;
    g_trampoline = nullptr;
    g_originalBuilder = nullptr;
    g_acquireHandle = nullptr;
    g_acquireMethodInfo = nullptr;
    AcquireSRWLockExclusive(&g_replacementsLock);
    g_markerHandles.clear();
    ReleaseSRWLockExclusive(&g_replacementsLock);
}

extern "C" __declspec(dllexport) void* __fastcall JacketBuilderBridge(
    void* outputHandle, void* resolverPipeline, void* assetReference, void* methodInfo) {
    InterlockedIncrement(&g_callCount);
    AsyncOperationHandle replacement{};
    bool hasReplacement = false;
    if (assetReference != nullptr) {
        AcquireSRWLockShared(&g_replacementsLock);
        const auto found = g_markerHandles.find(assetReference);
        if (found != g_markerHandles.end() && found->second.internalOperation != nullptr) {
            replacement = found->second;
            hasReplacement = true;
        }
        ReleaseSRWLockShared(&g_replacementsLock);
    }
    if (outputHandle != nullptr && hasReplacement) {
        return g_acquireHandle(static_cast<AsyncOperationHandle*>(outputHandle), &replacement, g_acquireMethodInfo);
    }
    JacketBuilder original = g_originalBuilder;

    return original == nullptr ? nullptr : original(outputHandle, resolverPipeline, assetReference, methodInfo);
}
