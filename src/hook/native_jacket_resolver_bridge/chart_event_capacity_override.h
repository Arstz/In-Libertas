#pragma once

#include <windows.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>

namespace chart_event_capacity {

struct CodeReader {
    BYTE* cursor;
    BYTE* end;

    bool consume(std::initializer_list<BYTE> bytes) {
        if (static_cast<SIZE_T>(end - cursor) < bytes.size()) return false;
        for (BYTE value : bytes) {
            if (*cursor++ != value) return false;
        }
        return true;
    }

    template<typename T> bool read(T& value) {
        if (static_cast<SIZE_T>(end - cursor) < sizeof(T)) return false;
        memcpy(&value, cursor, sizeof(T));
        cursor += sizeof(T);
        return true;
    }

    bool relativeTarget(BYTE*& target) {
        std::int32_t displacement = 0;
        if (!read(displacement)) return false;
        target = reinterpret_cast<BYTE*>(reinterpret_cast<std::uintptr_t>(cursor) + displacement);
        return true;
    }
};

struct Allocation {
    BYTE* operand = nullptr;
    int originalCapacity = 0;
    int fieldOffset = 0;
    int ownerRegister = 0;
    BYTE* arrayType = nullptr;
    BYTE* memoryMethod = nullptr;
    BYTE* allocator = nullptr;
    BYTE* memoryConstructor = nullptr;
};

struct AllocationPair {
    Allocation records;
    Allocation coordinates;
};

struct OverrideState {
    BYTE* constructor = nullptr;
    AllocationPair allocations;
    int capacity = 0;
};

static SRWLOCK g_lock = SRWLOCK_INIT;
static OverrideState g_state;

struct LockGuard {
    LockGuard() { AcquireSRWLockExclusive(&g_lock); }
    ~LockGuard() { ReleaseSRWLockExclusive(&g_lock); }
};

static bool accessible(const void* address, SIZE_T size, bool executable) {
    const auto first = reinterpret_cast<std::uintptr_t>(address);
    if (first == 0 || size == 0 || first + size < first) return false;
    auto cursor = first;
    while (cursor < first + size) {
        MEMORY_BASIC_INFORMATION region{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region)
            || region.State != MEM_COMMIT || (region.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
        const DWORD protection = region.Protect & 0xFF;
        const bool canExecute = protection == PAGE_EXECUTE_READ || protection == PAGE_EXECUTE_READWRITE
            || protection == PAGE_EXECUTE_WRITECOPY;
        if (executable ? !canExecute : !(canExecute || protection == PAGE_READONLY
                || protection == PAGE_READWRITE || protection == PAGE_WRITECOPY)) return false;
        const auto next = reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (next <= cursor) return false;
        cursor = next;
    }
    return true;
}

static bool constructorRange(BYTE* constructor, BYTE*& end) {
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(constructor), &imageBase, nullptr);
    if (function == nullptr || function->EndAddress <= function->BeginAddress
        || function->EndAddress - function->BeginAddress > 65536
        || imageBase + function->BeginAddress != reinterpret_cast<DWORD64>(constructor)) return false;
    end = reinterpret_cast<BYTE*>(imageBase + function->EndAddress);
    return accessible(constructor, static_cast<SIZE_T>(end - constructor), true);
}

static bool memoryStore(CodeReader& reader, int& ownerRegister, int& fieldOffset) {
    BYTE rex = 0, opcode = 0, modrm = 0;
    if (reader.cursor == reader.end) return false;
    if ((*reader.cursor & 0xF0) == 0x40 && !reader.read(rex)) return false;
    if ((rex & 0x06) != 0 || !reader.consume({0x0F, 0x11}) || !reader.read(modrm)
        || (modrm & 0x38) != 0) return false;
    const int mode = modrm >> 6;
    if (mode != 1 && mode != 2) return false;
    int base = modrm & 7;
    if (base == 4) {
        if (!reader.read(opcode) || (opcode & 0xF8) != 0x20) return false;
        base = opcode & 7;
    }
    ownerRegister = base + ((rex & 1) != 0 ? 8 : 0);
    if (ownerRegister == 4) return false;
    if (mode == 1) {
        std::int8_t offset = 0;
        if (!reader.read(offset)) return false;
        fieldOffset = offset;
    } else if (!reader.read(fieldOffset)) {
        return false;
    }
    return fieldOffset >= 0;
}

static bool stackAddress(CodeReader& reader, bool address, int& displacement) {
    BYTE modrm = 0;
    if (address) {
        if (!reader.consume({0x48, 0x8D})) return false;
    } else {
        BYTE opcode = 0;
        if (!reader.consume({0x0F}) || !reader.read(opcode) || (opcode != 0x28 && opcode != 0x10)) return false;
    }
    if (!reader.read(modrm) || !reader.consume({0x24})) return false;
    if (modrm == (address ? 0x4C : 0x44)) {
        std::int8_t offset = 0;
        if (!reader.read(offset)) return false;
        displacement = offset;
    } else if (modrm == (address ? 0x8C : 0x84)) {
        if (!reader.read(displacement)) return false;
    } else {
        return false;
    }
    return displacement >= 0;
}

static bool allocationAt(BYTE* start, BYTE* end, Allocation& allocation) {
    CodeReader reader{start, end};
    if (!reader.consume({0xBA})) return false;
    allocation.operand = reader.cursor;
    if (!reader.read(allocation.originalCapacity) || allocation.originalCapacity <= 0) return false;
    CodeReader precedingStore = reader;
    int unusedRegister = 0, unusedOffset = 0;
    if (memoryStore(precedingStore, unusedRegister, unusedOffset)) reader = precedingStore;
    if (!reader.consume({0x48, 0x8B, 0x0D}) || !reader.relativeTarget(allocation.arrayType)
        || !reader.consume({0xE8}) || !reader.relativeTarget(allocation.allocator)
        || !reader.consume({0x4C, 0x8B, 0x05}) || !reader.relativeTarget(allocation.memoryMethod)) return false;
    int outputOffset = 0, inputOffset = 0;
    if (!stackAddress(reader, true, outputOffset) || !reader.consume({0x48, 0x8B, 0xD0})
        || !reader.consume({0xE8}) || !reader.relativeTarget(allocation.memoryConstructor)
        || !stackAddress(reader, false, inputOffset) || outputOffset != inputOffset) return false;
    if (reader.cursor != end && *reader.cursor == 0xBA) {
        int nextCapacity = 0;
        if (!reader.consume({0xBA}) || !reader.read(nextCapacity)) return false;
    }
    return memoryStore(reader, allocation.ownerRegister, allocation.fieldOffset);
}

static bool findAllocations(BYTE* constructor, int recordField, int coordinateField, AllocationPair& pair) {
    if (recordField < 16 || coordinateField < 16
        || (static_cast<std::int64_t>(recordField) < static_cast<std::int64_t>(coordinateField) + 16
            && static_cast<std::int64_t>(coordinateField) < static_cast<std::int64_t>(recordField) + 16)) return false;
    BYTE* end = nullptr;
    if (!constructorRange(constructor, end)) return false;
    int records = 0, coordinates = 0;
    for (BYTE* cursor = constructor; cursor < end; ++cursor) {
        if (*cursor != 0xBA) continue;
        Allocation allocation;
        if (!allocationAt(cursor, end, allocation)) continue;
        if (allocation.fieldOffset == recordField) {
            if (++records != 1) return false;
            pair.records = allocation;
        } else if (allocation.fieldOffset == coordinateField) {
            if (++coordinates != 1) return false;
            pair.coordinates = allocation;
        }
    }
    return records == 1 && coordinates == 1
        && pair.records.ownerRegister == pair.coordinates.ownerRegister
        && pair.records.allocator == pair.coordinates.allocator
        && pair.records.memoryConstructor == pair.coordinates.memoryConstructor
        && pair.records.arrayType != pair.coordinates.arrayType
        && pair.records.memoryMethod != pair.coordinates.memoryMethod
        && accessible(pair.records.allocator, 1, true)
        && accessible(pair.records.memoryConstructor, 1, true)
        && accessible(pair.records.arrayType, sizeof(void*), false)
        && accessible(pair.coordinates.arrayType, sizeof(void*), false)
        && accessible(pair.records.memoryMethod, sizeof(void*), false)
        && accessible(pair.coordinates.memoryMethod, sizeof(void*), false);
}

static bool installedAllocationsMatch() {
    AllocationPair current;
    return findAllocations(g_state.constructor, g_state.allocations.records.fieldOffset,
            g_state.allocations.coordinates.fieldOffset, current)
        && current.records.operand == g_state.allocations.records.operand
        && current.coordinates.operand == g_state.allocations.coordinates.operand
        && current.records.originalCapacity == g_state.capacity
        && current.coordinates.originalCapacity == g_state.capacity;
}

static BOOL setCapacities(const AllocationPair& pair, int records, int coordinates) {
    DWORD recordProtection = 0, coordinateProtection = 0, ignored = 0;
    if (!VirtualProtect(pair.records.operand, sizeof(int), PAGE_EXECUTE_READWRITE, &recordProtection)) return FALSE;
    if (!VirtualProtect(pair.coordinates.operand, sizeof(int), PAGE_EXECUTE_READWRITE, &coordinateProtection)) {
        VirtualProtect(pair.records.operand, sizeof(int), recordProtection, &ignored);
        return FALSE;
    }
    memcpy(pair.records.operand, &records, sizeof(records));
    memcpy(pair.coordinates.operand, &coordinates, sizeof(coordinates));
    FlushInstructionCache(GetCurrentProcess(), pair.records.operand, sizeof(int));
    FlushInstructionCache(GetCurrentProcess(), pair.coordinates.operand, sizeof(int));
    VirtualProtect(pair.coordinates.operand, sizeof(int), coordinateProtection, &ignored);
    VirtualProtect(pair.records.operand, sizeof(int), recordProtection, &ignored);
    return TRUE;
}

} // namespace chart_event_capacity

extern "C" __declspec(dllexport) BOOL __fastcall InstallChartEventCapacityOverrideForFields(
    void* constructor, int capacity, int recordField, int coordinateField) {
    using namespace chart_event_capacity;
    LockGuard lock;
    if (constructor == nullptr || capacity != 100000) return FALSE;
    BYTE* source = static_cast<BYTE*>(constructor);
    if (g_state.constructor != nullptr) {
        return g_state.constructor == source && g_state.allocations.records.fieldOffset == recordField
            && g_state.allocations.coordinates.fieldOffset == coordinateField && installedAllocationsMatch();
    }
    AllocationPair pair;
    if (!findAllocations(source, recordField, coordinateField, pair)
        || pair.records.originalCapacity > capacity || pair.coordinates.originalCapacity > capacity) return FALSE;
    if (!setCapacities(pair, capacity, capacity)) return FALSE;
    g_state = {source, pair, capacity};
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL __fastcall InstallChartEventCapacityOverride(void* constructor, int capacity) {
    return InstallChartEventCapacityOverrideForFields(constructor, capacity, 0x50, 0xA0);
}

extern "C" __declspec(dllexport) BOOL __fastcall GetChartEventCapacityOverrideInfo(
    int* originalRecords, int* originalCoordinates, int* recordInstructionOffset, int* coordinateInstructionOffset) {
    using namespace chart_event_capacity;
    LockGuard lock;
    if (g_state.constructor == nullptr || originalRecords == nullptr || originalCoordinates == nullptr
        || recordInstructionOffset == nullptr || coordinateInstructionOffset == nullptr) return FALSE;
    *originalRecords = g_state.allocations.records.originalCapacity;
    *originalCoordinates = g_state.allocations.coordinates.originalCapacity;
    *recordInstructionOffset = static_cast<int>(g_state.allocations.records.operand - g_state.constructor - 1);
    *coordinateInstructionOffset = static_cast<int>(g_state.allocations.coordinates.operand - g_state.constructor - 1);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL __fastcall RemoveChartEventCapacityOverride() {
    using namespace chart_event_capacity;
    LockGuard lock;
    if (g_state.constructor == nullptr) return TRUE;
    if (!installedAllocationsMatch()
        || !setCapacities(g_state.allocations, g_state.allocations.records.originalCapacity,
            g_state.allocations.coordinates.originalCapacity)) return FALSE;
    g_state = {};
    return TRUE;
}
