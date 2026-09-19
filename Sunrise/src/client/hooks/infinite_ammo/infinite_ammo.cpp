/**
 * Infinite ammo. Three setters hold a weapon's supply, and each is asked for a full one rather than
 * written to, so a stored count's encoding stays out of this. Reserves do not clamp, so their
 * number is the number shown. A sword's setter does clamp; magazines use their largest observed
 * amount.
 */

#include "infinite_ammo.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::hooks::infinite_ammo {
namespace {

/** Call sites. Each setter starts with an obfuscated jump whose bytes repeat, so it is not scanned
 *  for directly. */
constexpr std::string_view kReservesCallText = "2B F5 48 8B CF 8B D6 E8 ? ? ? ?";
constexpr std::string_view kMagazineCallText = "8D 14 33 48 8B CF E8 ? ? ? ?";
/** Masked forms. These are what the image scan takes. */
constexpr auto kReservesCall =
    patterns::signature<patterns::signature_length(kReservesCallText)>(kReservesCallText);
/** Masked form of the magazine call site. */
constexpr auto kMagazineCall =
    patterns::signature<patterns::signature_length(kMagazineCallText)>(kMagazineCallText);

/** Offset of the call opcode inside each pattern above. */
constexpr std::size_t kReservesCallOffset = 7;
constexpr std::size_t kMagazineCallOffset = 6;
/** A near call is one opcode byte and a signed displacement. */
constexpr std::size_t kNearCallOperand = 1;
constexpr std::size_t kNearCallLength = 5;

/**
 * The sword setter, which has an ordinary entry. Its first instruction is `push rsi` with a
 * redundant REX prefix; a pattern starting a byte later matches inside the function.
 */
constexpr std::string_view kSwordText =
    "40 56 48 83 EC 50 0F 29 74 24 40 48 8B F1 0F 29 7C 24 30 48 83 C1 60";
/** Masked form of the sword setter. */
constexpr auto kSword = patterns::signature<patterns::signature_length(kSwordText)>(kSwordText);

/** Reserve count stored. No magazine holds over ~200, so the two read under 1000 together. */
constexpr std::int32_t kRequestedCount = 500;
/** Supply asked for on a sword. Its setter clamps this down to the sword's own maximum. */
constexpr float kRequestedSupply = 9999.0F;

using Setter = std::int64_t(__fastcall*)(void*, std::int32_t);
using SwordSetter = void(__fastcall*)(void*, float);

/** One slot each, so all three are attached together. */
constexpr std::size_t kHandleCount = 3;
constexpr std::size_t kReservesSlot = 0;
constexpr std::size_t kMagazineSlot = 1;
constexpr std::size_t kSwordSlot = 2;

std::array<hooking::detour::Handle, kHandleCount> g_handles{};

/**
 * The setter exposes the amount being stored, but not a separate magazine-capacity field. Keep a
 * bounded observation per weapon pointer and use its largest valid amount as that weapon's
 * capacity. A fixed table avoids retaining an unbounded number of stale game pointers.
 */
struct MagazineObservation {
    void* weapon{};
    std::int32_t capacity{};
    std::uint64_t lastSeen{};
};

constexpr std::size_t kMagazineObservationCount = 8;
std::array<MagazineObservation, kMagazineObservationCount> g_magazineObservations{};
SRWLOCK g_magazineObservationLock{SRWLOCK_INIT};
std::uint64_t g_observationSequence{};

/** @return True while the feature is on. */
[[nodiscard]] bool enabled() noexcept {
    return client::player::get().infiniteAmmoEnabled;
}

/** @return True when the setter argument can be a magazine capacity observation. */
[[nodiscard]] bool valid_magazine_amount(std::int32_t amount) noexcept {
    return amount > 0;
}

/**
 * Remembers the largest positive amount observed for one weapon pointer.
 * @param weapon Weapon instance supplied by the game.
 * @param amount Amount the game requested to store.
 * @return The known capacity, or the original amount when no pointer is available.
 */
[[nodiscard]] std::int32_t observe_magazine(void* weapon, std::int32_t amount) noexcept {
    if (weapon == nullptr || !valid_magazine_amount(amount)) {
        return amount;
    }

    AcquireSRWLockExclusive(&g_magazineObservationLock);
    std::size_t slot = kMagazineObservationCount;
    std::size_t replacement = 0;
    std::uint64_t oldest = std::numeric_limits<std::uint64_t>::max();
    for (std::size_t index = 0; index < g_magazineObservations.size(); ++index) {
        MagazineObservation& observation = g_magazineObservations[index];
        if (observation.weapon == weapon) {
            slot = index;
            break;
        }
        if (observation.weapon == nullptr) {
            replacement = index;
            oldest = 0;
            continue;
        }
        if (observation.lastSeen < oldest) {
            replacement = index;
            oldest = observation.lastSeen;
        }
    }
    if (slot == kMagazineObservationCount) {
        slot = replacement;
        g_magazineObservations[slot] = MagazineObservation{weapon, 0, 0};
    }

    MagazineObservation& observation = g_magazineObservations[slot];
    if (amount > observation.capacity) {
        observation.capacity = amount;
    }
    observation.lastSeen = ++g_observationSequence;
    const std::int32_t capacity = observation.capacity;
    ReleaseSRWLockExclusive(&g_magazineObservationLock);
    return capacity;
}

/**
 * Asks for a full reserve instead of the amount the game worked out.
 * @param weapon Weapon instance.
 * @param amount Amount the caller wanted to store.
 * @return Whatever the original returns.
 */
std::int64_t __fastcall set_reserves(void* weapon, std::int32_t amount) noexcept {
    const Setter next = reinterpret_cast<Setter>(g_handles[kReservesSlot].original);
    if (next == nullptr) {
        return 0;
    }
    return next(weapon, enabled() ? kRequestedCount : amount);
}

/**
 * Observes the magazine amount and optionally holds it at the largest value seen for this weapon.
 * The reserve setter is still called exactly as before when Infinite Reserves is enabled.
 * @param weapon Weapon instance.
 * @param amount Amount the caller wanted to store.
 * @return Whatever the original returns.
 */
std::int64_t __fastcall set_magazine(void* weapon, std::int32_t amount) noexcept {
    const Setter next = reinterpret_cast<Setter>(g_handles[kMagazineSlot].original);
    if (next == nullptr) {
        return 0;
    }
    const client::player::Settings settings = client::player::get();
    const std::int32_t knownCapacity = observe_magazine(weapon, amount);
    const std::int32_t requestedAmount = settings.infiniteMagazineEnabled
                                              ? knownCapacity
                                              : amount;
    const std::int64_t result = next(weapon, requestedAmount);
    const Setter reserves = reinterpret_cast<Setter>(g_handles[kReservesSlot].original);
    if (enabled() && reserves != nullptr && weapon != nullptr) {
        (void)reserves(weapon, kRequestedCount);
    }
    return result;
}

/**
 * Keeps a sword's supply full. Its own clamp decides how full.
 * @param weapon Weapon instance.
 * @param supply Supply the caller wanted to store.
 */
void __fastcall set_sword_supply(void* weapon, float supply) noexcept {
    const SwordSetter next = reinterpret_cast<SwordSetter>(g_handles[kSwordSlot].original);
    if (next == nullptr) {
        return;
    }
    next(weapon, enabled() ? kRequestedSupply : supply);
}

/**
 * Decodes the setter a call site targets.
 * @param site Base of the matched call-site pattern.
 * @param offset Offset of the call opcode inside that pattern.
 * @return The setter, or null when the site does not resolve.
 */
[[nodiscard]] std::byte* setter_from(std::byte* site, std::size_t offset) noexcept {
    if (site == nullptr) {
        return nullptr;
    }
    std::byte* const call = site + offset;
    return static_cast<std::byte*>(
        patterns::resolve_relative(call + kNearCallOperand, call + kNearCallLength));
}

/** @param reason Key naming the step that failed. @return False, for a direct return. */
[[nodiscard]] bool fail(const char* reason) noexcept {
    std::array<char, 96> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=infinite_ammo stage=install result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Attaches to all three setters. The magazine one also observes and optionally holds its amount. */
bool install() noexcept {
    if (g_handles[kReservesSlot].original != nullptr) {
        return true;
    }
    std::byte* const reserves =
        setter_from(patterns::scan_main_image_unique(kReservesCall, "infinite_ammo_reserves"),
                    kReservesCallOffset);
    if (reserves == nullptr) {
        return fail("reserves");
    }
    std::byte* const magazine =
        setter_from(patterns::scan_main_image_unique(kMagazineCall, "infinite_ammo_magazine"),
                    kMagazineCallOffset);
    if (magazine == nullptr) {
        return fail("magazine");
    }
    std::byte* const sword = patterns::scan_main_image_unique(kSword, "infinite_ammo_sword");
    if (sword == nullptr) {
        return fail("sword");
    }
    const std::array<hooking::detour::Spec, kHandleCount> specs{
        hooking::detour::Spec{reserves, reinterpret_cast<void*>(&set_reserves)},
        hooking::detour::Spec{magazine, reinterpret_cast<void*>(&set_magazine)},
        hooking::detour::Spec{sword, reinterpret_cast<void*>(&set_sword_supply)},
    };
    if (!hooking::detour::install(specs, g_handles)) {
        return fail("attach");
    }
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=infinite_ammo stage=install result=ok");
    return true;
}

/** Detaches every detour. */
void uninstall() noexcept {
    if (g_handles[kReservesSlot].original == nullptr) {
        return;
    }
    (void)hooking::detour::uninstall(g_handles);
    g_handles = {};
    AcquireSRWLockExclusive(&g_magazineObservationLock);
    g_magazineObservations = {};
    g_observationSequence = 0;
    ReleaseSRWLockExclusive(&g_magazineObservationLock);
}

} // namespace sunrise::client::hooks::infinite_ammo
