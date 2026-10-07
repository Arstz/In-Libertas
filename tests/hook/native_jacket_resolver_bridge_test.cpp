#include "../../src/hook/native_jacket_resolver_bridge/native_jacket_resolver_bridge.cpp"
#include <cstdio>

static int g_acquireCount = 0;

static AsyncOperationHandle* __fastcall acquireFixture(
    AsyncOperationHandle* output, const AsyncOperationHandle* source, void* methodInfo) {
    if (methodInfo != &g_acquireCount) return nullptr;
    ++g_acquireCount;
    *output = *source;

    return output;
}

static bool verify(bool condition, const char* message) {
    if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);

    return condition;
}

int main() {
    constexpr BYTE kFixtureTail[] = {0x5B, 0x48, 0x8B, 0xC1, 0xC3};
    constexpr SIZE_T kFixtureSize = 64;
    BYTE* fixture = static_cast<BYTE*>(VirtualAlloc(
        nullptr, kFixtureSize, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    AsyncOperationHandle replacement{&g_acquireCount, 7, 0, fixture};
    AsyncOperationHandle output{};
    int marker = 0;
    int stockReference = 0;
    bool passed = fixture != nullptr;

    if (!passed) return 1;
    passed &= verify(GetJacketBuilderBridgeApiVersion() == kBridgeApiVersion, "paired bridge API version");
    passed &= verify(!InstallJacketBuilderBridge(fixture, acquireFixture, &g_acquireCount), "reject unknown prologue");
    passed &= verify(fixture[0] == 0, "rejected target remains unchanged");
    memcpy(fixture, kPipelinePrologue, sizeof(kPipelinePrologue));
    memcpy(fixture + sizeof(kPipelinePrologue), kFixtureTail, sizeof(kFixtureTail));
    FlushInstructionCache(GetCurrentProcess(), fixture, kFixtureSize);
    passed &= verify(!InstallJacketBuilderBridge(fixture, nullptr, &g_acquireCount), "reject missing Acquire entry");
    passed &= verify(InstallJacketBuilderBridge(fixture, acquireFixture, &g_acquireCount), "install validated trampoline");
    RegisterJacketReplacement(&marker, nullptr, &replacement, nullptr);
    JacketBuilder target = reinterpret_cast<JacketBuilder>(fixture);
    passed &= verify(target(&output, nullptr, &stockReference, nullptr) == &output, "stock trampoline return");
    passed &= verify(output.internalOperation == nullptr && g_acquireCount == 0, "stock request stays untouched");
    for (int index = 0; index < 3; ++index) {
        passed &= verify(target(&output, nullptr, &marker, nullptr) == &output, "marker return");
        passed &= verify(memcmp(&output, &replacement, sizeof(output)) == 0, "full 24-byte handle substitution");
    }
    passed &= verify(g_acquireCount == 3, "one Acquire per returned replacement");
    RemoveJacketBuilderBridge();
    passed &= verify(memcmp(fixture, kPipelinePrologue, sizeof(kPipelinePrologue)) == 0, "restore original prologue");
    passed &= verify(target(&output, nullptr, &stockReference, nullptr) == &output, "restored target return");
    VirtualFree(fixture, 0, MEM_RELEASE);
    if (passed) std::puts("Native jacket bridge regression checks passed.");

    return passed ? 0 : 1;
}
