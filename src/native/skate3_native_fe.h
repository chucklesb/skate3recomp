#pragma once
// Native frontend-state methods (skate3_native_fe.cpp) that other hooks call.
#include <cstdint>

#include <rex/ppc/context.h>

namespace skate3::native_fe {

// FrontEndState_Options::IsItemEnabled (sub_8260D648) for the game's own
// rows: the native body, or the recompiled one when skate3_native_fe_replace
// is off.
void OptionsItemEnabled(PPCContext& ctx, uint8_t* base);

}  // namespace skate3::native_fe
