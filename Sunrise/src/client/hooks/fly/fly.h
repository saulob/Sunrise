#pragma once

namespace sunrise::client::hooks::fly {

/**
 * Fastest speed the game is shown while this hook drives the position itself. Contact with
 * geometry damages the player above roughly this, and the real speed is not needed for movement.
 */
inline constexpr float kPublishedSpeedCap = 8.0F;

/**
 * Finds the game's processed left-stick move vector, which is read every tick alongside the keys.
 * A miss is logged and leaves the stick contribution at zero. Movement speed uses the stick;
 * fly continues to fly from the keys alone.
 */
void resolve_controller() noexcept;

/** Drops the stick source. The next tick uses the keys alone. */
void clear_controller() noexcept;

/** Reads the fly and movement speed toggle keys once a frame and flips each switch on its press. */
void poll_toggle() noexcept;

/**
 * Writes the player's velocity on the physics sync, which publishes it.
 * @param component Physics component being synced. Tested for player ownership here.
 */
void apply(void* component) noexcept;

/** @return True while fly is on. */
[[nodiscard]] bool enabled() noexcept;

/**
 * Sets the velocity the coming simulation step integrates. Noclip reads it too.
 * @param body Character rigid body. Live only inside the step hook.
 */
void before_step(void* body) noexcept;

/**
 * Puts back the height the step's gravity took.
 * @param body Character rigid body. Live only inside the step hook.
 * @param heldElsewhere True when noclip carries the vertical lane.
 */
void after_step(void* body, bool heldElsewhere) noexcept;

/**
 * Movement speed: the same keys and camera-relative direction as fly, plus the controller's left
 * stick, at a configured speed, on the horizontal lanes only. The vertical lane stays the game's,
 * so gravity, jumping and falling are untouched.
 * @return True while movement speed drives the horizontal lanes: on, with fly off. Noclip may
 * be on; it then carries this speed through geometry.
 */
[[nodiscard]] bool speed_enabled() noexcept;

/**
 * Sets the horizontal velocity the coming simulation step integrates.
 * @param body Character rigid body. Live only inside the step hook.
 */
void before_speed_step(void* body) noexcept;

/**
 * Caps the horizontal speed the game is shown after the step, as fly does for all three lanes.
 * @param body Character rigid body. Live only inside the step hook.
 */
void after_speed_step(void* body) noexcept;

/**
 * Writes the capped horizontal velocity on the physics sync, which publishes it.
 * @param component Physics component being synced. Tested for player ownership here.
 */
void apply_speed(void* component) noexcept;

/** Clears the key state. The switches are stored settings and survive. */
void reset() noexcept;

} // namespace sunrise::client::hooks::fly
