#pragma once

#include <cstdint>
#include <functional>

namespace rex::runtime {
class FunctionDispatcher;
}
namespace rex::input {
class InputSystem;
}

namespace skate3::demo_path {

void InstallHooks(rex::runtime::FunctionDispatcher* dispatcher);
// Provider for the merged UI pad state consumed by the user-facing frontend
// movie skip. Set during app setup, before the guest boots (the provider is
// read from the guest thread without further synchronization).
void SetUiInputProvider(std::function<rex::input::InputSystem*()> provider);
// The provider's current input system, or null before app setup. For other
// host features that poll the merged UI pad state.
rex::input::InputSystem* GetUiInputSystem();
bool ShouldForceIntroMovieComplete();
// The game's XInputGetState hook reports what player one's pad gave the game
// (after any harness rewrite), for the intro skip.
void NoteGuestPad(uint8_t* base, uint32_t user, uint32_t state_addr, uint32_t result);
// A fresh press that skips the current intro screen (skate3_intro_movie_skip;
// any button before the title screen, A / Start after). One edge detector
// shared with the movie skip, polled once per screen update.
bool IntroSkipPressed();

}  // namespace skate3::demo_path
