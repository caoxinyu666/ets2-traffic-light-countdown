#pragma once

// Game-memory layout and signatures adapted from ETS2LA/plugin (MIT License,
// copyright 2024 Dario Wouters). See third_party/ETS2LA-plugin-LICENSE.md.
// Targets ETS2 Windows x64 1.61.1.9; fail closed when a signature is absent.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

struct NativeSignal {
    double x = 0, z = 0;
    float remaining = 0;
    uint32_t state = 0;
};

class NativeSignalReader {
    uintptr_t basePointerAddress = 0;
    uint32_t nearbyOffset = 0;
    uintptr_t gameBase = 0;
    size_t gameSize = 0;

    template<class T> static bool read(uintptr_t address, T& out) {
        MEMORY_BASIC_INFORMATION info{};
        if (!address || !VirtualQuery(reinterpret_cast<void*>(address), &info, sizeof(info)) ||
            info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
            address + sizeof(T) < address ||
            address + sizeof(T) > reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize) return false;
        memcpy(&out, reinterpret_cast<const void*>(address), sizeof(T));
        return true;
    }

    static uintptr_t scan(const uint8_t* start, size_t length, const int* pattern, size_t count) {
        if (length < count) return 0;
        for (size_t i = 0; i <= length - count; ++i) {
            if (start[i] != pattern[0]) continue;
            size_t j = 1;
            while (j < count && (pattern[j] < 0 || start[i + j] == pattern[j])) ++j;
            if (j == count) return reinterpret_cast<uintptr_t>(start + i);
        }
        return 0;
    }

    struct ArrayHeader { uintptr_t vtable, data; uint64_t size; };

public:
    bool initialize() {
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        if (!base) return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
        const auto* pe = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (pe->Signature != IMAGE_NT_SIGNATURE) return false;
        gameBase = base;
        gameSize = pe->OptionalHeader.SizeOfImage;
        const auto* section = IMAGE_FIRST_SECTION(pe);
        const uint8_t* code = nullptr;
        size_t length = 0;
        for (unsigned i = 0; i < pe->FileHeader.NumberOfSections; ++i) {
            if (section[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) {
                code = reinterpret_cast<const uint8_t*>(base + section[i].VirtualAddress);
                length = section[i].Misc.VirtualSize;
                break;
            }
        }
        if (!code) return false;
        const int basePattern[] = {0x48,0x8b,-1,-1,-1,-1,-1,0x48,0x8b,-1,0x48,0x8b,-1,0x48,0x8b,0x80};
        const int nearbyPattern[] = {0x4c,0x8d,0x81,-1,-1,-1,-1,0x0f,0x29,-1,-1,0x48,0x8d,0x91};
        const uintptr_t a = scan(code, length, basePattern, std::size(basePattern));
        const uintptr_t b = scan(code, length, nearbyPattern, std::size(nearbyPattern));
        if (!a || !b) return false;
        int32_t displacement = 0;
        if (!read(a + 3, displacement) || !read(b + 3, nearbyOffset)) return false;
        basePointerAddress = a + 7 + displacement;
        return nearbyOffset > 0x100 && nearbyOffset < 0x10000;
    }

    unsigned poll(std::array<NativeSignal, 256>& result) const {
        if (!basePointerAddress) return 0;
        uintptr_t controller = 0;
        if (!read(basePointerAddress, controller)) return 0;
        ArrayHeader nearby{};
        if (!read(controller + nearbyOffset, nearby) || nearby.size > 4096) return 0;
        unsigned count = 0;
        for (uint64_t i = 0; i < nearby.size && count < result.size(); ++i) {
            uintptr_t item = 0;
            uint8_t type = 0;
            if (!read(nearby.data + i * 8, item) || !read(item + 0x0a, type) || type != 4) continue;
            uintptr_t segment = 0;
            if (!read(item + 0x48, segment)) continue;
            ArrayHeader semaphores{};
            if (!read(segment + 0x80, semaphores) || semaphores.size > 512) continue;
            for (uint64_t j = 0; j < semaphores.size && count < result.size(); ++j) {
                uintptr_t actor = 0, rule = 0;
                const uintptr_t record = semaphores.data + j * 16;
                if (!read(record + 8, actor) || !read(actor + 0x100, rule)) continue;
                uintptr_t vtable = 0, typeFunction = 0;
                if (!read(rule, vtable) || !read(vtable + 8, typeFunction) ||
                    typeFunction < gameBase || typeFunction >= gameBase + gameSize) continue;
                using GetType = uint64_t (*)(void*);
                if (reinterpret_cast<GetType>(typeFunction)(reinterpret_cast<void*>(rule)) != 0x2000) continue;
                uint32_t state = 0;
                float remaining = 0, x = 0, z = 0;
                int16_t cx = 0, cz = 0;
                if (!read(rule + 0x70, state) || !read(rule + 0xb8, remaining) ||
                    !read(actor + 0x28, x) || !read(actor + 0x30, z) ||
                    !read(actor + 0x34, cx) || !read(actor + 0x36, cz)) continue;
                if ((state != 1 && state != 2 && state != 4 && state != 8) ||
                    !std::isfinite(remaining) || remaining <= 0 || remaining > 300) continue;
                result[count++] = {x + cx * 512.0, z + cz * 512.0, remaining, state};
            }
        }
        return count;
    }
};
