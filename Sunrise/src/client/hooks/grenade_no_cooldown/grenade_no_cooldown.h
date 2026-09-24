#pragma once

namespace sunrise::client::hooks::grenade_no_cooldown {

/**
 * Resolves the current-ability getter, the handle tables and the native ability-energy
 * adjustment, and attaches the getter observer that records the Grenade owner.
 * @return True when every target resolved and the detour attached, or it already was.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the getter observer. */
void uninstall() noexcept;

/**
 * While Grenade No Cooldown is on, adds one full unit of grenade energy through the game's own
 * adjustment. Does nothing while it is off. Call it per frame, on the game thread.
 */
void poll() noexcept;

} // namespace sunrise::client::hooks::grenade_no_cooldown
