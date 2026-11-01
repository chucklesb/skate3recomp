#pragma once

#include <functional>

// The recomp settings in the game's own Options screen (FrontEndState_Options,
// options.apt): Display, Graphics, View, PC Controls and System rows on the
// root Options screen open pages of native selector rows, and All PC Settings
// opens the full host settings overlay, and Quit Game exits after a Quit /
// Cancel prompt. Restart-required settings are staged and applied through a
// Restart Now / Discard Changes prompt on Back.
namespace skate3::options_menu {

// Adds the rows to the root Options layout. Call once after the XEX image is
// loaded and before the guest launches. No-op (logged) when the image does not
// match the expected title update layout or the cvar is off.
void Install();

// The callbacks are invoked from the guest thread and must marshal to the UI
// thread themselves. Set during app setup, before the guest boots.
// Open: the "All PC Settings" row was selected.
void SetOpenSettingsCallback(std::function<void()> callback);
// Save: settings rows changed cvars; persist the user settings file.
void SetSaveSettingsCallback(std::function<void()> callback);
// Restart: restart-required settings were applied (and saved); restart the
// game. Invoked after the save callback.
void SetRestartCallback(std::function<void()> callback);
// Quit: Quit Game was confirmed (settings saved first); close the game the
// way the overlay's Close Game does. Invoked after the save callback.
void SetQuitCallback(std::function<void()> callback);

}  // namespace skate3::options_menu
