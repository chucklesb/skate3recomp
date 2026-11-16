#pragma once
// Guest CPU profiler (RE-7 target ranking): samples the game's main and
// render threads and writes the stacks as module+offset, for
// tools/harness/profile_report.py to symbolize against the build
// (recompiled functions are sub_XXXXXXXX symbols in skate3).
//
// - Windows: a sampler thread suspends each registered thread, reads its
//   context, unwinds up to kMaxDepth frames (RtlVirtualUnwind) and resumes
//   it. A sample where the thread used no CPU cycles since the last one is
//   counted as idle, not as a stack.
// - Linux: a per-thread CPU-time timer (timer_create on the thread's CPU
//   clock, SIGEV_THREAD_ID) sends SIGPROF to the thread itself; the handler
//   records PC, LR (aarch64) and a frame-pointer walk bounded to the
//   thread's stack, into a lock-free ring a drain thread empties.
//
// Threads register themselves (NoteThread) from a hook that runs on them;
// the harness does it for the main thread (game tick) and the render thread
// (guest swap).
#include <cstdint>
#include <filesystem>
#include <string>

namespace skate3::harness::profile {

// Call on the thread to be sampled (cheap after the first call).
void NoteThread(const char* name);

bool Active();
// Starts sampling the registered threads (and any that register later) at
// `hz` samples per second of the thread's CPU time (Linux) or wall time
// (Windows, idle samples dropped).
bool Start(int hz, std::string* error);
// Stops and writes <dir>/profile.txt. `frames` = game frames sampled over
// (for ms-per-frame figures). Returns a JSON fragment for result.json
// ("profile":{...}), empty if nothing was running.
std::string Stop(const std::filesystem::path& dir, uint64_t frames);

}  // namespace skate3::harness::profile
