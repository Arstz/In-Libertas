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

static constexpr BYTE kLocalizedResolverCode[] = {
    0x83, 0xEA, 0x01, 0x74, 0x26, 0x83, 0xEA, 0x01,
    0x74, 0x1B, 0x83, 0xEA, 0x01, 0x74, 0x1C, 0x83,
    0xEA, 0x01, 0x74, 0x0B, 0x83, 0xFA, 0x01, 0x75,
    0x12, 0x48, 0x8B, 0x41, 0x20, 0xEB, 0x0F, 0x48,
    0x8B, 0x41, 0x18, 0xEB, 0x09, 0x48, 0x8B, 0x41,
    0x08, 0xEB, 0x03, 0x48, 0x8B, 0x01, 0x48, 0x85,
    0xC0, 0x74, 0x06, 0x83, 0x78, 0x10, 0x00, 0x75,
    0x03, 0x48, 0x8B, 0x01, 0xC3
};

struct LocalizedDisplayStrings {
    void* english;
    void* japanese;
    void* korean;
    void* traditionalChinese;
    void* simplifiedChinese;
};
static_assert(sizeof(LocalizedDisplayStrings) == 40);

static BYTE* g_localizedResolverTarget = nullptr;
static BYTE g_localizedResolverOriginal[16]{};

extern "C" __declspec(dllexport) void* __fastcall ResolveLocalizedDisplayString(
    const LocalizedDisplayStrings* values, int localization, void*) {
    if (values == nullptr) return nullptr;
    void* selected = values->english;
    switch (localization) {
    case 2: selected = values->japanese; break;
    case 3: selected = values->korean; break;
    case 4: selected = values->traditionalChinese; break;
    case 5: selected = values->simplifiedChinese; break;
    }
    if (selected == nullptr || *reinterpret_cast<const int*>(static_cast<const BYTE*>(selected) + 16) == 0)
        return values->english;
    return selected;
}

extern "C" __declspec(dllexport) BOOL __fastcall InstallLocalizedStringResolver(void* target) {
    if (g_localizedResolverTarget != nullptr) return g_localizedResolverTarget == target;
    if (target == nullptr || memcmp(target, kLocalizedResolverCode, sizeof(kLocalizedResolverCode)) != 0)
        return FALSE;
    BYTE* source = static_cast<BYTE*>(target);
    DWORD oldProtection = 0;
    if (!VirtualProtect(source, sizeof(g_localizedResolverOriginal), PAGE_EXECUTE_READWRITE, &oldProtection))
        return FALSE;
    memcpy(g_localizedResolverOriginal, source, sizeof(g_localizedResolverOriginal));
    writeAbsoluteJump(source, reinterpret_cast<void*>(&ResolveLocalizedDisplayString));
    source[14] = 0x90;
    source[15] = 0x90;
    DWORD ignored = 0;
    VirtualProtect(source, sizeof(g_localizedResolverOriginal), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), source, sizeof(g_localizedResolverOriginal));
    g_localizedResolverTarget = source;
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL __fastcall RemoveLocalizedStringResolver() {
    if (g_localizedResolverTarget == nullptr) return TRUE;
    DWORD oldProtection = 0;
    if (!VirtualProtect(g_localizedResolverTarget, sizeof(g_localizedResolverOriginal), PAGE_EXECUTE_READWRITE, &oldProtection))
        return FALSE;
    memcpy(g_localizedResolverTarget, g_localizedResolverOriginal, sizeof(g_localizedResolverOriginal));
    DWORD ignored = 0;
    VirtualProtect(g_localizedResolverTarget, sizeof(g_localizedResolverOriginal), oldProtection, &ignored);
    FlushInstructionCache(GetCurrentProcess(), g_localizedResolverTarget, sizeof(g_localizedResolverOriginal));
    g_localizedResolverTarget = nullptr;
    return TRUE;
}

#include "chart_event_capacity_override.h"
