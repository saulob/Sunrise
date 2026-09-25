/** The player module's interface. Every control saves at once, so a change survives a restart. */

#include "player_panel.h"

#include <imgui.h>

#include "../../../core/ui/components/toggle/ui_toggle_component.h"
#include "../../player/player_settings_store.h"

namespace sunrise::client::ui::player {

/** Draws the player module inside the active Core UI frame. */
void draw() noexcept {
    namespace toggle = core::ui::components::toggle;
    client::player::Settings settings = client::player::get();

    ImGui::TextUnformatted("Infinite Ammo");
    ImGui::Separator();
    ImGui::TextWrapped("Keep weapon ammunition full.");
    ImGui::Spacing();
    bool changed =
        toggle::control("Infinite Reserves##infinite_ammo", settings.infiniteAmmoEnabled);
    changed = toggle::control("Infinite Magazine##infinite_magazine",
                              settings.infiniteMagazineEnabled)
              || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Anti AFK");
    ImGui::Separator();
    ImGui::TextWrapped("Disable AFK timeouts from activities kicking to orbit and the title "
                       "screen.");
    ImGui::Spacing();
    changed = toggle::control("Enabled##anti_afk", settings.antiAfkEnabled) || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("No Damage");
    ImGui::Separator();
    ImGui::TextWrapped("Prevent combat, fall, Turn Back and out-of-bounds damage.");
    ImGui::Spacing();
    changed = toggle::control("Enabled##no_damage", settings.noDamageEnabled) || changed;

    ImGui::Spacing();
    ImGui::Spacing();
    ImGui::TextUnformatted("Abilities");
    ImGui::Separator();
    ImGui::TextWrapped("Remove ability cooldown after use.");
    ImGui::Spacing();
    changed = toggle::control("Grenade##grenade_no_cooldown", settings.grenadeNoCooldownEnabled)
              || changed;
    changed = toggle::control("Melee##melee_no_cooldown", settings.meleeNoCooldownEnabled)
              || changed;
    changed = toggle::control("Class Ability##class_ability_no_cooldown",
                              settings.classAbilityNoCooldownEnabled)
              || changed;
    changed = toggle::control("Super##super_no_cooldown", settings.superNoCooldownEnabled)
              || changed;

    if (changed) {
        (void)client::player::publish(settings);
    }
}

} // namespace sunrise::client::ui::player
