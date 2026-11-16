#pragma once
// D3D fence waits without spinning (RE-7; battery and heat on handhelds).
//
// D3D::CDevice::BlockOnFence (sub_82B755C0) and the other D3D waits loop on
// sub_82B76080, one poll: ~32 db16cyc spin hints, then a read of the GPU's
// completed-fence word *(device+10896) against the value seen last poll
// (state+8), with GPU-deadlock detection after a long stall. On the host the
// loop burns a core (Odin 2 uncapped: 0.80 ms of CPU per frame, 39% of the
// render thread; docs/re/hot-functions.md).
//
// The fence word is written by the emulated command processor, which now
// signals every guest-memory write-back (rex::graphics::NotifyGpuWriteback).
// With skate3_native_fence_wait = on, the poll hook blocks until the next
// write-back when the word has not moved since the last poll (2 ms timeout
// as a safety net), then runs the original poll unchanged (deadlock checks,
// return values). fence: lines count polls, waits, wakes and timeouts.
namespace skate3::native_fence {

// Frame tail: the stats line, cvar refresh.
void OnFrameBuilt();

}  // namespace skate3::native_fence
