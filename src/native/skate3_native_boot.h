#pragma once
// The boot flow's frontend screens, native (RE-8; docs/re/boot.md).
//
// The boot runs Sk8::Flow::StateBootFlow's handler chain: language select
// (FE state 47, auto-accepts after 10 s), the Skate 3 logo (FE 5, 4 s), the
// Black Box movie (FE 24), a wait for the boot load, then the title (FE 22).
// Every screen is timed in milliseconds of frame dt, not in frames; the
// boot load only starts once the movie plays, so the earlier screens delay
// it.
//
// skate3_native_boot_replace (default on): the logo's OnUpdate
// (sub_82608E38) as a Body family `boot_logo`: the same 4000 ms timer on the
// state's +60 accumulator, setting the done byte +64 that the boot flow
// waits on; and, with skate3_intro_movie_skip, a fresh press of any button
// ends it the same way.
namespace skate3::native_boot {}
