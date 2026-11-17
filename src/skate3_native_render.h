#pragma once

// Skate 3 native renderer (data-driven scene renderer).
//
// The guest-function hooks in skate3_native_render.cpp are link-time
// weak-symbol overrides and are always present; everything they do is gated
// behind the skate3_native_render cvar. Install() only announces state and
// validates configuration. The scene capture/build and rendering live in
// skate3_native_scene.cpp.

namespace skate3::native_render {

void Install();

// The frame cap skate3_fast_loading lifted on the frame being finished (0
// when the frame was paced or there is no cap). Guest render thread; the
// harness reads it right after the pacer, to keep frame-count waits in
// capped-frame time while loads run uncapped.
double LiftedCapThisFrame();

}  // namespace skate3::native_render
