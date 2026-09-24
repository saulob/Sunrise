/**
 * Grenade and Melee No Cooldown use the game's Change Ability Energy path (sandbox action kind 8).
 *
 * The game's executor for that action (decrypted runtime code around RVA 0xEC4E3F..0xEC50C0):
 * 1. calls the current-ability getter (RVA 0xB9CFA0) with the owner and selected slot;
 * 2. builds a 0x28-byte component reference on its stack from the copied entry, resolving the
 *    two handles at entry +0x10 and +0x18 through the process handle tables;
 * 3. adjusts the energy with `adjust(&reference, amount, 2, 1.0f)` (RVA 0x186A870; the 1.0f
 *    is the .rdata constant at RVA 0x1BA2B80).
 *
 * This module records owners passed for slots 0 and 2, then maintains each enabled slot with
 * one full unit of energy. A temporary read-only trace observes the known BA2030 state accessor.
 */

#include "grenade_no_cooldown.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <intrin.h>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../content/handles/layout.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::hooks::grenade_no_cooldown {
namespace {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;
namespace layout = content::handles::layout;

/** The current-ability getter's own entry, from the decrypted runtime bytes at RVA 0xB9CFA0. */
constexpr std::string_view kGetterText =
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B F1 49 63 F8 48 8B CA 48 8B DA E8 ? ? ? ? "
    "33 D2 4C 8D 0D ? ? ? ? 8B CF 44 8D 42 07 E8 ? ? ? ? 84 C0 74 1A 48 8D 14 7F 48 C1 E2 04 48 "
    "8D 8E E0 02 00 00";
constexpr auto kGetter = signature<signature_length(kGetterText)>(kGetterText);

/**
 * The executor's getter call (RVA 0xEC4EAE) and the reference build that follows it, up to the
 * handle-tables load `mov r8,[rip+..]` (RVA 0xEC4EE1).
 */
constexpr std::string_view kReferenceText =
    "48 8D 94 24 80 00 00 00 48 23 41 08 48 2B C8 E8 ? ? ? ? 0F 10 50 10 F2 0F 10 40 20 0F 11 54 "
    "24 68 66 0F 73 DA 08 66 41 0F 7E D1 F2 0F 11 44 24 78 41 83 F9 FF 0F 84 ? ? ? ? 8B 54 24 68 "
    "8B C2 4C 8B 05 ? ? ? ? 81 E2 FF 1F 00 00 0F 10 44 24 70 C1 F8 0D";
constexpr auto kReference = signature<signature_length(kReferenceText)>(kReferenceText);
constexpr std::size_t kReferenceGetterCall = 15;
constexpr std::size_t kReferenceTablesLoad = 66;

/**
 * The executor's energy read (RVA 0xEC501D), its limit clamp, and the energy adjustment
 * (RVA 0xEC5080..0xEC5098).
 */
constexpr std::string_view kAdjustText =
    "48 8D 4C 24 30 E8 ? ? ? ? 0F 2E 05 ? ? ? ? 7A 05 75 03 0F 57 C0 0F 2F F7 76 1F F3 0F 10 4B "
    "0C 0F 2F C8 76 39 F3 0F 5C C8 0F 28 C6 F3 0F 5C C1 0F 2F C7 72 2C 0F 28 F1 EB 27 0F 2F FE 76 "
    "22 F3 0F 10 4B 0C 0F 2F C1 76 15 F3 0F 5C C8 0F 28 C6 F3 0F 5C C1 0F 2F C7 73 08 0F 28 F1 EB "
    "03 0F 57 F6 F3 0F 10 1D ? ? ? ? 48 8D 4C 24 30 41 B0 02 0F 28 CE E8 ? ? ? ?";
constexpr auto kAdjust = signature<signature_length(kAdjustText)>(kAdjustText);
constexpr std::size_t kAdjustConstantLoad = 99;
constexpr std::size_t kAdjustCall = 118;

/** A near call is one opcode byte and a rel32; a RIP-relative load here is 3 + rel32. */
constexpr std::size_t kCallLength = 5;
constexpr std::size_t kLoadOperand = 4;
constexpr std::size_t kLoadLength = 8;
constexpr std::size_t kMovLoadOperand = 3;
constexpr std::size_t kMovLoadLength = 7;
constexpr std::byte kCallOpcode{0xE8};

/** Action kind 8 targets Grenade at slot 0 and Melee at slot 2. */
constexpr std::int32_t kGrenadeSlot = 0;
constexpr std::int32_t kMeleeSlot = 2;
/** The executor's mode byte for this adjustment. */
constexpr std::uint8_t kAdjustMode = 2;
/** One full unit of energy; the executor compares energy against the same 1.0f constant. */
constexpr float kFullEnergy = 1.0F;
/** An owner the getter has not reported for this long is not trusted. */
constexpr std::uint64_t kOwnerFreshMs = 2000;
constexpr std::uintptr_t kEnergyReaderRva = 0x186AF20;
constexpr std::uintptr_t kActiveStateRva = 0xBA2030;
constexpr std::size_t kActiveStateOffset = 0x640;
constexpr unsigned kTraceSampleBudget = 200;
constexpr unsigned kTraceRepeatInterval = 32;

/** Copied ability entry: owner + 0x2E0 + slot * 0x30. */
constexpr std::size_t kEntrySize = 0x30;
constexpr std::size_t kEntryInterfaceHandle = 0x10;
constexpr std::size_t kEntryComponentHandle = 0x18;
constexpr std::size_t kEntryComponentOffset = 0x20;
constexpr std::uint32_t kNoHandle = 0xFFFFFFFF;

/** Handle decoding, identical to the executor and to content::handles::resolve. */
constexpr unsigned kRecordIndexBits = 13;
constexpr std::uint32_t kRecordIndexMask = (1U << kRecordIndexBits) - 1U;
constexpr std::uint64_t kTableMaskFill = 0x0FFC0000ULL;
constexpr unsigned kTableMaskShift = 18;

/** The executor's stack reference at [rsp+30h]; the reader and the adjustment take its address. */
struct alignas(16) ComponentReference {
    std::uintptr_t interfaceObject{};
    std::uintptr_t component{};
    std::array<std::byte, 16> entryTail{};
    std::uint8_t cleared{};
    std::array<std::byte, 15> padding{};
};
static_assert(offsetof(ComponentReference, component) == 0x08);
static_assert(offsetof(ComponentReference, entryTail) == 0x10);
static_assert(offsetof(ComponentReference, cleared) == 0x20);

using CurrentGetter = void*(__fastcall*)(void*, void*, std::int32_t);
using EnergyAdjust = void(__fastcall*)(ComponentReference*, float, std::uint8_t, float);
using EnergyRead = float(__fastcall*)(ComponentReference*);
using ActiveState = std::uint8_t(__fastcall*)(void*);

hooking::detour::Handle g_handle{};
hooking::detour::Handle g_stateHandle{};
EnergyAdjust g_adjust{nullptr};
EnergyRead g_energyRead{nullptr};
const float* g_adjustRange{nullptr};
std::byte* g_tablesSlot{nullptr};
std::uintptr_t g_mainBase{0};

std::atomic<void*> g_grenadeOwner{nullptr};
std::atomic<std::uint64_t> g_grenadeOwnerTick{0};
std::atomic<void*> g_meleeOwner{nullptr};
std::atomic<std::uint64_t> g_meleeOwnerTick{0};

/** Frame thread only. */
bool g_grenadeWasEnabled{false};
bool g_grenadeAppliedLogged{false};
bool g_grenadeSkipLogged{false};
bool g_meleeWasEnabled{false};
bool g_meleeAppliedLogged{false};
bool g_meleeSkipLogged{false};

/** The trace receives only a fresh current-Grenade reference published by poll(). */
std::atomic<bool> g_traceEnabled{false};
std::atomic<std::uintptr_t> g_traceInterface{0};
std::atomic<std::uintptr_t> g_traceComponent{0};
std::atomic<float> g_traceEnergy{0.0F};
std::atomic<bool> g_traceEnergyAvailable{false};
std::atomic<bool> g_traceExhausted{false};
std::atomic_flag g_traceGuard = ATOMIC_FLAG_INIT;
bool g_traceBegun{false};
bool g_traceHaveLast{false};
bool g_traceLastStateAvailable{false};
std::uint8_t g_traceLastState{0};
std::uint8_t g_traceLastReturn{0};
std::uintptr_t g_traceLastCaller{0};
unsigned g_traceMatchingCalls{0};
unsigned g_traceSamples{0};
unsigned g_traceChanges{0};

template <typename T> [[nodiscard]] bool read_at(std::uintptr_t address, T& value) noexcept {
    if (address == 0) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(),
                             reinterpret_cast<const void*>(address),
                             &value,
                             sizeof value,
                             &read)
               != FALSE
           && read == sizeof value;
}

/** @return True when every page of the range is committed and readable. */
[[nodiscard]] bool readable(std::uintptr_t address, std::size_t size) noexcept {
    std::uintptr_t cursor = address;
    const std::uintptr_t end = address + size;
    while (cursor < end) {
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof info) != sizeof info
            || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
            return false;
        }
        cursor = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    }
    return true;
}

/**
 * Resolves one handle the way the executor does, without the content-table floor that
 * content::handles::resolve applies (ability components live in generic tables).
 * @param record Receives the record address.
 * @param correction Receives the native correction the executor subtracts.
 */
[[nodiscard]] bool resolve(std::uint32_t handle,
                           std::uintptr_t& record,
                           std::uintptr_t& correction) noexcept {
    std::uintptr_t tablesObject = 0;
    std::uintptr_t tableBase = 0;
    if (!read_at(reinterpret_cast<std::uintptr_t>(g_tablesSlot), tablesObject)
        || !read_at(tablesObject, tableBase) || tableBase == 0) {
        return false;
    }
    // `sar eax,0Dh`, then the table index from the selector's own width mask.
    const auto encodedHigh =
        static_cast<std::uint32_t>(static_cast<std::int32_t>(handle) >> kRecordIndexBits);
    const std::uint64_t tableMask =
        (static_cast<std::uint64_t>(encodedHigh) | kTableMaskFill) >> kTableMaskShift;
    const std::uint64_t tableIndex = static_cast<std::uint16_t>(encodedHigh) & tableMask;
    layout::TableDescriptor table{};
    if (!read_at(tableBase + tableIndex * sizeof(layout::TableDescriptor), table)
        || table.recordArray == 0) {
        return false;
    }
    // `imul edx,[table+30h]` is a 32-bit product, zero-extended before the add.
    record = table.recordArray
             + static_cast<std::uint32_t>((handle & kRecordIndexMask) * table.recordStride);
    layout::RecordPrefix prefix{};
    if (!read_at(record, prefix)) {
        return false;
    }
    correction = prefix.correctionSource
                 & static_cast<std::uint64_t>(static_cast<std::int64_t>(table.correctionMask));
    return true;
}

/** Builds the reference the executor builds for one current-ability slot. */
[[nodiscard]] bool build_reference(void* owner,
                                   std::int32_t slot,
                                   ComponentReference& reference) noexcept {
    const auto getter = reinterpret_cast<CurrentGetter>(g_handle.original);
    if (getter == nullptr) {
        return false;
    }
    alignas(16) std::array<std::byte, kEntrySize> entry{};
    (void)getter(owner, entry.data(), slot);
    std::uint32_t interfaceHandle = 0;
    std::uint32_t componentHandle = 0;
    std::uint64_t componentOffset = 0;
    std::memcpy(&interfaceHandle, entry.data() + kEntryInterfaceHandle, sizeof interfaceHandle);
    std::memcpy(&componentHandle, entry.data() + kEntryComponentHandle, sizeof componentHandle);
    std::memcpy(&componentOffset, entry.data() + kEntryComponentOffset, sizeof componentOffset);
    // `cmp r9d,-1 / je`: the executor skips a slot with no component.
    if (componentHandle == kNoHandle) {
        return false;
    }
    std::uintptr_t record = 0;
    std::uintptr_t correction = 0;
    if (!resolve(interfaceHandle, record, correction)) {
        return false;
    }
    reference.interfaceObject = record - correction;
    if (!resolve(componentHandle, record, correction)) {
        return false;
    }
    reference.component = record - correction + componentOffset;
    std::memcpy(reference.entryTail.data(),
                entry.data() + kEntryComponentHandle,
                reference.entryTail.size());
    reference.cleared = 0;
    return reference.interfaceObject != 0 && reference.component != 0
           && readable(reference.interfaceObject, sizeof(std::uintptr_t) * 4)
           && readable(reference.component, sizeof(std::uintptr_t));
}

void log_once(bool& flag,
               const char* feature,
               const char* stage,
               const char* detail) noexcept {
    if (flag) {
        return;
    }
    flag = true;
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "DEBUG_SAULO ev=%s stage=%s %s",
                      feature,
                      stage,
                      detail);
}

/** Passes calls through and records owners for the two supported current-ability slots. */
void* __fastcall current_getter(void* owner, void* output, std::int32_t slot) noexcept {
    const auto next = reinterpret_cast<CurrentGetter>(g_handle.original);
    void* const result = next != nullptr ? next(owner, output, slot) : output;
    const std::uint64_t now = GetTickCount64();
    if (owner != nullptr && slot == kGrenadeSlot) {
        g_grenadeOwner.store(owner, std::memory_order_release);
        g_grenadeOwnerTick.store(now, std::memory_order_release);
    } else if (owner != nullptr && slot == kMeleeSlot) {
        g_meleeOwner.store(owner, std::memory_order_release);
        g_meleeOwnerTick.store(now, std::memory_order_release);
    }
    return result;
}

void clear_trace_reference() noexcept {
    while (g_traceGuard.test_and_set(std::memory_order_acquire)) {
    }
    g_traceEnabled.store(false, std::memory_order_release);
    g_traceInterface.store(0, std::memory_order_release);
    g_traceComponent.store(0, std::memory_order_release);
    g_traceEnergyAvailable.store(false, std::memory_order_release);
    g_traceGuard.clear(std::memory_order_release);
}

void publish_trace_reference(const ComponentReference& reference) noexcept {
    while (g_traceGuard.test_and_set(std::memory_order_acquire)) {
    }
    g_traceComponent.store(reference.component, std::memory_order_release);
    g_traceInterface.store(reference.interfaceObject, std::memory_order_release);
    g_traceEnabled.store(true, std::memory_order_release);
    g_traceGuard.clear(std::memory_order_release);
}

void trace_state_call(void* object, std::uintptr_t caller, std::uint8_t result) noexcept {
    if (!g_traceEnabled.load(std::memory_order_acquire)
        || object == nullptr
        || reinterpret_cast<std::uintptr_t>(object)
               != g_traceInterface.load(std::memory_order_acquire)
        || g_traceComponent.load(std::memory_order_acquire) == 0
        || g_traceExhausted.load(std::memory_order_acquire)) {
        return;
    }
    if (g_traceGuard.test_and_set(std::memory_order_acquire)) {
        return;
    }
    if (!g_traceEnabled.load(std::memory_order_acquire)
        || reinterpret_cast<std::uintptr_t>(object)
               != g_traceInterface.load(std::memory_order_acquire)
        || g_traceComponent.load(std::memory_order_acquire) == 0
        || g_traceExhausted.load(std::memory_order_acquire)) {
        g_traceGuard.clear(std::memory_order_release);
        return;
    }

    std::uint8_t state = 0;
    const bool stateAvailable = read_at(reinterpret_cast<std::uintptr_t>(object)
                                            + kActiveStateOffset,
                                        state);
    const std::uint64_t now = GetTickCount64();
    std::uintptr_t callerRva = 0;
    if (g_mainBase != 0 && caller >= g_mainBase) {
        callerRva = caller - g_mainBase;
    }

    if (!g_traceBegun) {
        g_traceBegun = true;
        core::log::writef(core::log::Channel::client,
                          core::log::Level::info,
                          "DEBUG_SAULO ev=grenade_recovery_trace stage=begin t=%llu object=0x%llX "
                          "caller=+0x%llX",
                          static_cast<unsigned long long>(now),
                          static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(object)),
                          static_cast<unsigned long long>(callerRva));
    }

    ++g_traceMatchingCalls;
    const bool stateChanged = g_traceHaveLast
                              && (stateAvailable != g_traceLastStateAvailable
                                  || (stateAvailable && state != g_traceLastState)
                                  || result != g_traceLastReturn);
    const bool firstSample = !g_traceHaveLast;
    const bool periodic = g_traceMatchingCalls % kTraceRepeatInterval == 0;
    if ((firstSample || stateChanged || periodic)
        && g_traceSamples < kTraceSampleBudget) {
        if (stateChanged) {
            ++g_traceChanges;
        }
        const std::uintptr_t objectAddress = reinterpret_cast<std::uintptr_t>(object);
        const bool energyAvailable = g_traceEnergyAvailable.load(std::memory_order_acquire);
        const float energy = g_traceEnergy.load(std::memory_order_acquire);
        if (stateAvailable && energyAvailable) {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "DEBUG_SAULO ev=grenade_recovery_trace stage=sample t=%llu "
                              "object=0x%llX state640=%u ret=%u caller=+0x%llX energy=%.6f",
                              static_cast<unsigned long long>(now),
                              static_cast<unsigned long long>(objectAddress),
                              static_cast<unsigned>(state),
                              static_cast<unsigned>(result),
                              static_cast<unsigned long long>(callerRva),
                              static_cast<double>(energy));
        } else if (stateAvailable) {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "DEBUG_SAULO ev=grenade_recovery_trace stage=sample t=%llu "
                              "object=0x%llX state640=%u ret=%u caller=+0x%llX "
                              "energy=unavailable",
                              static_cast<unsigned long long>(now),
                              static_cast<unsigned long long>(objectAddress),
                              static_cast<unsigned>(state),
                              static_cast<unsigned>(result),
                              static_cast<unsigned long long>(callerRva));
        } else if (energyAvailable) {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "DEBUG_SAULO ev=grenade_recovery_trace stage=sample t=%llu "
                              "object=0x%llX state640=unavailable ret=%u caller=+0x%llX "
                              "energy=%.6f",
                              static_cast<unsigned long long>(now),
                              static_cast<unsigned long long>(objectAddress),
                              static_cast<unsigned>(result),
                              static_cast<unsigned long long>(callerRva),
                              static_cast<double>(energy));
        } else {
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "DEBUG_SAULO ev=grenade_recovery_trace stage=sample t=%llu "
                              "object=0x%llX state640=unavailable ret=%u caller=+0x%llX "
                              "energy=unavailable",
                              static_cast<unsigned long long>(now),
                              static_cast<unsigned long long>(objectAddress),
                              static_cast<unsigned>(result),
                              static_cast<unsigned long long>(callerRva));
        }
        ++g_traceSamples;
        g_traceLastStateAvailable = stateAvailable;
        g_traceLastState = state;
        g_traceLastReturn = result;
        g_traceLastCaller = callerRva;
        g_traceHaveLast = true;
        if (g_traceSamples == kTraceSampleBudget) {
            g_traceExhausted.store(true, std::memory_order_release);
            core::log::writef(core::log::Channel::client,
                              core::log::Level::info,
                              "DEBUG_SAULO ev=grenade_recovery_trace stage=end samples=%u changes=%u",
                              g_traceSamples,
                              g_traceChanges);
        }
    }
    g_traceGuard.clear(std::memory_order_release);
}

/** Read-only observer for the proven BA2030 active-state accessor. */
__declspec(noinline) std::uint8_t __fastcall active_state(void* object) noexcept {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const auto next = reinterpret_cast<ActiveState>(g_stateHandle.original);
    const std::uint8_t result = next != nullptr ? next(object) : 0;
    trace_state_call(object, caller, result);
    return result;
}

/** @param reason Step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    core::log::writef(core::log::Channel::client,
                      core::log::Level::warn,
                      "ev=grenade_no_cooldown stage=install result=fail reason=%s",
                      reason);
    return false;
}

} // namespace

bool install() noexcept {
    if (g_handle.original != nullptr) {
        return true;
    }
    std::byte* const getter = scan_main_image_unique(kGetter, "grenade_no_cooldown_getter");
    if (getter == nullptr) {
        return fail("getter");
    }
    std::byte* const referenceSite =
        scan_main_image_unique(kReference, "grenade_no_cooldown_reference");
    if (referenceSite == nullptr || referenceSite[kReferenceGetterCall] != kCallOpcode) {
        return fail("reference");
    }
    // The executor's own getter call must land on the getter found above.
    std::byte* const called = resolve_relative(referenceSite + kReferenceGetterCall + 1,
                                               referenceSite + kReferenceGetterCall + kCallLength);
    if (called != getter) {
        return fail("getter_mismatch");
    }
    std::byte* const tablesSlot =
        resolve_relative(referenceSite + kReferenceTablesLoad + kMovLoadOperand,
                         referenceSite + kReferenceTablesLoad + kMovLoadLength);
    std::byte* const adjustSite = scan_main_image_unique(kAdjust, "grenade_no_cooldown_adjust");
    if (adjustSite == nullptr || adjustSite[kAdjustCall] != kCallOpcode) {
        return fail("adjust");
    }
    std::byte* const adjust = resolve_relative(adjustSite + kAdjustCall + 1,
                                               adjustSite + kAdjustCall + kCallLength);
    std::byte* const range = resolve_relative(adjustSite + kAdjustConstantLoad + kLoadOperand,
                                              adjustSite + kAdjustConstantLoad + kLoadLength);
    if (tablesSlot == nullptr || adjust == nullptr || range == nullptr) {
        return fail("decode");
    }
    g_tablesSlot = tablesSlot;
    g_adjust = reinterpret_cast<EnergyAdjust>(adjust);
    g_adjustRange = reinterpret_cast<const float*>(range);
    if (!hooking::detour::install(
            hooking::detour::Spec{getter, reinterpret_cast<void*>(&current_getter)}, g_handle)) {
        return fail("attach");
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=grenade_no_cooldown stage=install result=ok");

    // These fixed RVAs are the already-decoded current-build reader and state accessor. The
    // trace hook is optional and never prevents either energy option from installing.
    HMODULE const module = GetModuleHandleW(nullptr);
    if (module != nullptr) {
        g_mainBase = reinterpret_cast<std::uintptr_t>(module);
        g_energyRead = reinterpret_cast<EnergyRead>(g_mainBase + kEnergyReaderRva);
        auto* const stateTarget = reinterpret_cast<std::byte*>(g_mainBase + kActiveStateRva);
        if (!hooking::detour::install(
                hooking::detour::Spec{stateTarget, reinterpret_cast<void*>(&active_state)},
                g_stateHandle)) {
            g_energyRead = nullptr;
            core::log::write(core::log::Channel::client,
                             core::log::Level::warn,
                             "DEBUG_SAULO ev=grenade_recovery_trace stage=install result=fail");
        } else {
            core::log::write(core::log::Channel::client,
                             core::log::Level::info,
                             "DEBUG_SAULO ev=grenade_recovery_trace stage=install result=ok");
        }
    } else {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "DEBUG_SAULO ev=grenade_recovery_trace stage=install result=fail reason=no_module");
    }
    return true;
}

void uninstall() noexcept {
    clear_trace_reference();
    if (g_stateHandle.original != nullptr) {
        (void)hooking::detour::uninstall(g_stateHandle);
        g_stateHandle = {};
    }
    if (g_handle.original == nullptr) {
        return;
    }
    (void)hooking::detour::uninstall(g_handle);
    g_handle = {};
}

void poll() noexcept {
    const client::player::Settings settings = client::player::get();
    const bool grenadeEnabled = settings.grenadeNoCooldownEnabled;
    if (grenadeEnabled != g_grenadeWasEnabled) {
        g_grenadeWasEnabled = grenadeEnabled;
        g_grenadeAppliedLogged = false;
        g_grenadeSkipLogged = false;
        if (grenadeEnabled) {
            while (g_traceGuard.test_and_set(std::memory_order_acquire)) {
            }
            g_traceBegun = false;
            g_traceHaveLast = false;
            g_traceLastStateAvailable = false;
            g_traceLastState = 0;
            g_traceLastReturn = 0;
            g_traceLastCaller = 0;
            g_traceMatchingCalls = 0;
            g_traceSamples = 0;
            g_traceChanges = 0;
            g_traceExhausted.store(false, std::memory_order_release);
            g_traceGuard.clear(std::memory_order_release);
        }
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         grenadeEnabled ? "DEBUG_SAULO ev=grenade_no_cooldown stage=enabled"
                                        : "DEBUG_SAULO ev=grenade_no_cooldown stage=disabled");
    }

    const bool meleeEnabled = settings.meleeNoCooldownEnabled;
    if (meleeEnabled != g_meleeWasEnabled) {
        g_meleeWasEnabled = meleeEnabled;
        g_meleeAppliedLogged = false;
        g_meleeSkipLogged = false;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         meleeEnabled ? "DEBUG_SAULO ev=melee_no_cooldown stage=enabled"
                                      : "DEBUG_SAULO ev=melee_no_cooldown stage=disabled");
    }

    clear_trace_reference();

    constexpr std::size_t kSlotArrayEnd = 0x2E0 + 8 * kEntrySize;
    const auto maintain = [&](bool enabled,
                              std::int32_t slot,
                              std::atomic<void*>& ownerStorage,
                              std::atomic<std::uint64_t>& tickStorage,
                              const char* feature,
                              bool& appliedLogged,
                              bool& skipLogged,
                              ComponentReference* output) noexcept {
        if (output != nullptr) {
            *output = {};
        }
        if (!enabled || g_adjust == nullptr || g_adjustRange == nullptr) {
            return false;
        }
        void* const owner = ownerStorage.load(std::memory_order_acquire);
        const std::uint64_t seen = tickStorage.load(std::memory_order_acquire);
        if (owner == nullptr || GetTickCount64() - seen > kOwnerFreshMs) {
            log_once(skipLogged, feature, "skip", "reason=no_fresh_owner");
            return false;
        }
        if (!readable(reinterpret_cast<std::uintptr_t>(owner), kSlotArrayEnd)) {
            log_once(skipLogged, feature, "skip", "reason=owner_unreadable");
            return false;
        }
        ComponentReference reference{};
        if (!build_reference(owner, slot, reference)) {
            log_once(skipLogged, feature, "skip", "reason=reference");
            return false;
        }
        if (output != nullptr) {
            *output = reference;
            if (slot == kGrenadeSlot && g_energyRead != nullptr) {
                g_traceEnergy.store(g_energyRead(&reference), std::memory_order_release);
                g_traceEnergyAvailable.store(true, std::memory_order_release);
            }
        }
        g_adjust(&reference, kFullEnergy, kAdjustMode, *g_adjustRange);
        log_once(appliedLogged,
                 feature,
                 "apply",
                 slot == kGrenadeSlot ? "writer=resolved slot=0 amount=1.0"
                                      : "writer=resolved slot=2 amount=1.0");
        return true;
    };

    ComponentReference grenadeReference{};
    const bool grenadeReferenceValid = maintain(grenadeEnabled,
                                                kGrenadeSlot,
                                                g_grenadeOwner,
                                                g_grenadeOwnerTick,
                                                "grenade_no_cooldown",
                                                g_grenadeAppliedLogged,
                                                g_grenadeSkipLogged,
                                                &grenadeReference);
    if (grenadeReferenceValid) {
        publish_trace_reference(grenadeReference);
    }

    (void)maintain(meleeEnabled,
                   kMeleeSlot,
                   g_meleeOwner,
                   g_meleeOwnerTick,
                   "melee_no_cooldown",
                   g_meleeAppliedLogged,
                   g_meleeSkipLogged,
                   nullptr);
}

} // namespace sunrise::client::hooks::grenade_no_cooldown
