/**
 * No damage. One native transaction applies a damage packet's resulting health fractions to a
 * health component. While the feature is on, a transaction aimed at the object the local player
 * controls is dropped before it runs, so health, shields, regeneration and death state are never
 * written by this module. Every other transaction passes through unchanged.
 */

#include "no_damage.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"
#include "../teleport/runtime.h"

namespace sunrise::client::hooks::no_damage {
namespace {

/**
 * Call site of the damage transaction. The transaction starts with an obfuscated jump, so it is
 * not scanned for directly. The site encodes the whole argument setup, so a match implies the ABI.
 */
constexpr std::string_view kTransactionCallText =
    "C7 44 24 30 FF FF FF FF 44 0F B6 CB 48 C7 44 24 28 00 00 00 00 4C 8B C7 48 8B D6 88 44 24 "
    "20 E8 ? ? ? ?";
/** Masked form. This is what the image scan takes. */
constexpr auto kTransactionCall =
    patterns::signature<patterns::signature_length(kTransactionCallText)>(kTransactionCallText);

/** Offset of the call opcode inside the pattern above. */
constexpr std::size_t kTransactionCallOffset = 31;
/** A near call is one opcode byte and a signed displacement. */
constexpr std::size_t kNearCallOperand = 1;
constexpr std::size_t kNearCallLength = 5;

/** The health component the transaction targets, inside its first argument. */
constexpr std::size_t kContextComponent = 8;
/** Object handle of the entity a health component belongs to. */
constexpr std::size_t kComponentOwnerHandle = 0x2C;
/** The controlled-object getter writes this when the local player owns no object. */
constexpr std::uint32_t kInvalidHandle = 0xFFFFFFFF;

/**
 * The transaction. Its first argument is a context whose +8 is the health component, and the
 * packet carries resulting fractions rather than damage amounts.
 */
using Transaction = void(__fastcall*)(const void* context,
                                      const void* damage,
                                      void* packet,
                                      bool mode,
                                      bool secondary,
                                      const void* extra,
                                      std::int32_t index);

hooking::detour::Handle g_handle{};

/** @return True while the feature is on. */
[[nodiscard]] bool enabled() noexcept {
    return client::player::get().noDamageEnabled;
}

/**
 * Reads one value out of game memory without faulting on a torn pointer.
 * @param address Source address.
 * @param value Receives the value.
 * @return True when Windows copied the whole value.
 */
template <typename T> [[nodiscard]] bool read_at(const void* address, T& value) noexcept {
    if (address == nullptr) {
        return false;
    }
    SIZE_T read = 0;
    return ReadProcessMemory(GetCurrentProcess(), address, &value, sizeof value, &read) != FALSE
           && read == sizeof value;
}

/**
 * @param context The transaction's first argument.
 * @return True when the health component belongs to the object the local player controls.
 *
 * Strict equality of the whole handle: the generation bits rule out a recycled object slot.
 */
[[nodiscard]] bool targets_local_player(const void* context) noexcept {
    std::uint32_t controlled = kInvalidHandle;
    if (!teleport::controlled_handle(controlled)) {
        return false;
    }
    const std::byte* component = nullptr;
    std::uint32_t owner = kInvalidHandle;
    return read_at(static_cast<const std::byte*>(context) + kContextComponent, component)
           && read_at(component + kComponentOwnerHandle, owner) && owner == controlled;
}

/**
 * Drops a transaction aimed at the local player while the feature is on, and otherwise defers
 * to the original unchanged.
 */
void __fastcall apply(const void* context,
                      const void* damage,
                      void* packet,
                      bool mode,
                      bool secondary,
                      const void* extra,
                      std::int32_t index) noexcept {
    const Transaction next = reinterpret_cast<Transaction>(g_handle.original);
    if (next == nullptr) {
        return;
    }
    if (enabled() && targets_local_player(context)) {
        return;
    }
    next(context, damage, packet, mode, secondary, extra, index);
}

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=no_damage stage=install result=fail reason=%s",
                                      reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Attaches to the damage transaction. */
bool install() noexcept {
    if (g_handle.original != nullptr) {
        return true;
    }
    std::byte* const site =
        patterns::scan_main_image_unique(kTransactionCall, "no_damage_transaction");
    if (site == nullptr) {
        return fail("call_site");
    }
    std::byte* const call = site + kTransactionCallOffset;
    std::byte* const transaction =
        patterns::resolve_relative(call + kNearCallOperand, call + kNearCallLength);
    if (transaction == nullptr) {
        return fail("transaction");
    }
    const hooking::detour::Spec spec{transaction, reinterpret_cast<void*>(&apply)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail("attach");
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=no_damage stage=install result=ok");
    return true;
}

/** Detaches the detour. */
void uninstall() noexcept {
    if (g_handle.original == nullptr) {
        return;
    }
    if (hooking::detour::uninstall(g_handle)) {
        g_handle = {};
    }
}

} // namespace sunrise::client::hooks::no_damage
