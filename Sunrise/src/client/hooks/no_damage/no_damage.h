#pragma once

namespace sunrise::client::hooks::no_damage {

/**
 * Attaches to the damage transaction. It is attached whether or not the feature is on, so the
 * Player control can switch it without a restart.
 * @return True when the transaction resolved and the detour attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches the detour. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::no_damage
