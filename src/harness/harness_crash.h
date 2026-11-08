#pragma once

// Crash capture for harness runs: on a fault nothing else handles, write
// <run>/crash.txt (and <run>/crash.dmp on Windows) before the process dies.
//
// crash.txt is line-oriented so tools/harness/run_scenario.py can parse it:
//   crash: <what>
//   address: 0x<faulting data address>       (access violations)
//   frame: <module>+0x<offset>               (innermost first)
//   frame: 0x<absolute pc>                   (when the module is unknown)
//   maps:                                    (POSIX: /proc/self/maps follows)
// Module offsets are from the module's load base (the RVA on Windows), which
// is what `run_scenario.py symbolize` takes.

#include <filesystem>

namespace skate3::harness::crash {

// Arm crash capture for this run. Paths are fixed here because the handlers
// may not allocate.
void Install(const std::filesystem::path& run_dir);

// The `crash` scenario verb: fault on purpose to test the capture.
void FaultNow();          // on the calling thread
void ArmGuestFault();     // on the next guest pad poll (a guest thread)
void MaybeGuestFault();   // called from the pad hook

}  // namespace skate3::harness::crash
