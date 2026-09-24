/**
 * Grenade No Cooldown, done the way Sundial's "Change Ability Energy" (sandbox perk action kind 8,
 * class 0x80803E4D, selector 0 = Grenade) is carried out by the game.
 *
 * The game's executor for that action (decrypted runtime code around RVA 0xEC4E3F..0xEC50C0):
 * 1. calls the current-ability getter (RVA 0xB9CFA0) with the owner and slot 0;
 * 2. builds a 0x28-byte component reference on its stack from the copied entry, resolving the
 *    two handles at entry +0x10 and +0x18 through the process handle tables;
 * 3. adjusts the energy with `adjust(&reference, amount, 2, 1.0f)` (RVA 0x186A870; the 1.0f
 *    is the .rdata constant at RVA 0x1BA2B80).
 *
 * This module records the owner the game itself passes for slot 0, then, while the setting is
 * on, repeats steps 1-3 every frame with one full unit of energy. With the setting off it does
 * nothing but pass the getter through.
 */

#include "grenade_no_cooldown.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
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

/** Grenade is ability slot 0 (Sundial selector enum, 0x80803E4D key 2). */
constexpr std::int32_t kGrenadeSlot = 0;
/** The executor's mode byte for this adjustment. */
constexpr std::uint8_t kAdjustMode = 2;
/** One full unit of energy; the executor compares energy against the same 1.0f constant. */
constexpr float kFullEnergy = 1.0F;
/** An owner the getter has not reported for this long is not trusted. */
constexpr std::uint64_t kOwnerFreshMs = 2000;

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

hooking::detour::Handle g_handle{};
EnergyAdjust g_adjust{nullptr};
const float* g_adjustRange{nullptr};
std::byte* g_tablesSlot{nullptr};

std::atomic<void*> g_owner{nullptr};
std::atomic<std::uint64_t> g_ownerTick{0};

/** Frame thread only. */
bool g_wasEnabled{false};
bool g_appliedLogged{false};
bool g_skipLogged{false};

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

/** Builds the reference the executor builds at [rsp+30h] for the Grenade slot. */
[[nodiscard]] bool build_reference(void* owner, ComponentReference& reference) noexcept {
    const auto getter = reinterpret_cast<CurrentGetter>(g_handle.original);
    if (getter == nullptr) {
        return false;
    }
    alignas(16) std::array<std::byte, kEntrySize> entry{};
    (void)getter(owner, entry.data(), kGrenadeSlot);
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

void log_once(bool& flag, const char* stage, const char* detail) noexcept {
    if (flag) {
        return;
    }
    flag = true;
    core::log::writef(core::log::Channel::client,
                      core::log::Level::info,
                      "DEBUG_SAULO ev=grenade_no_cooldown stage=%s %s",
                      stage,
                      detail);
}

/** Passes every call through; records the owner the game uses for the Grenade slot. */
void* __fastcall current_getter(void* owner, void* output, std::int32_t slot) noexcept {
    const auto next = reinterpret_cast<CurrentGetter>(g_handle.original);
    void* const result = next != nullptr ? next(owner, output, slot) : output;
    if (slot == kGrenadeSlot && owner != nullptr) {
        g_owner.store(owner, std::memory_order_release);
        g_ownerTick.store(GetTickCount64(), std::memory_order_release);
    }
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
    return true;
}

void uninstall() noexcept {
    if (g_handle.original == nullptr) {
        return;
    }
    (void)hooking::detour::uninstall(g_handle);
    g_handle = {};
}

void poll() noexcept {
    const bool enabled = client::player::get().grenadeNoCooldownEnabled;
    if (enabled != g_wasEnabled) {
        g_wasEnabled = enabled;
        g_appliedLogged = false;
        g_skipLogged = false;
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         enabled ? "DEBUG_SAULO ev=grenade_no_cooldown stage=enabled"
                                 : "DEBUG_SAULO ev=grenade_no_cooldown stage=disabled");
    }
    if (!enabled || g_adjust == nullptr || g_adjustRange == nullptr) {
        return;
    }
    void* const owner = g_owner.load(std::memory_order_acquire);
    const std::uint64_t seen = g_ownerTick.load(std::memory_order_acquire);
    if (owner == nullptr || GetTickCount64() - seen > kOwnerFreshMs) {
        log_once(g_skipLogged, "skip", "reason=no_fresh_owner");
        return;
    }
    // The getter reads the owner's slot array; a stale owner must not reach it.
    constexpr std::size_t kSlotArrayEnd = 0x2E0 + 8 * kEntrySize;
    if (!readable(reinterpret_cast<std::uintptr_t>(owner), kSlotArrayEnd)) {
        log_once(g_skipLogged, "skip", "reason=owner_unreadable");
        return;
    }
    ComponentReference reference{};
    if (!build_reference(owner, reference)) {
        log_once(g_skipLogged, "skip", "reason=reference");
        return;
    }
    g_adjust(&reference, kFullEnergy, kAdjustMode, *g_adjustRange);
    log_once(g_appliedLogged, "apply", "writer=resolved slot=0 amount=1.0");
}

} // namespace sunrise::client::hooks::grenade_no_cooldown
