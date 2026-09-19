#pragma once

namespace sunrise::client::hooks::infinite_ammo {

/**
 * Attaches to the reserve, magazine and sword setters. The magazine amount is observed and can be
 * held at the largest valid amount seen for that weapon while Infinite Magazine is enabled.
 * @return True when all three resolved and the detours attached.
 */
[[nodiscard]] bool install() noexcept;

/** Detaches every detour. */
void uninstall() noexcept;

} // namespace sunrise::client::hooks::infinite_ammo
