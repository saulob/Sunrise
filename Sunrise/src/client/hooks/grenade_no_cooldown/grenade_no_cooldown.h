#pragma once

namespace sunrise::client::hooks::grenade_no_cooldown {

/**
 * Resolves the current-ability getter, the handle tables and the native ability-energy
 * adjustment, and attaches the getter observer that records Grenade and Melee owners. It also
 * installs the temporary read-only trace on the known BA2030 state accessor when available.
 * @return True when every required energy target resolved and the getter detour attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the getter observer. */
void uninstall() noexcept;

/**
 * While either energy option is on, adds one full unit to that ability through the game's own
 * adjustment. When Grenade No Cooldown is on, publishes the current Grenade reference for the
 * temporary read-only BA2030 trace. Call per frame on the game thread.
 */
void poll() noexcept;

} // namespace sunrise::client::hooks::grenade_no_cooldown
