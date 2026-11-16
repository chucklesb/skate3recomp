# Hot guest functions (RE-7 ranking)

Where the game's CPU time goes, measured with the harness profiler
(`profile` verb, `tools/harness/profile_report.py`;
[scenario-harness.md](../scenario-harness.md)). It ranks candidates for
native replacement (RE-7) by cost; the other half of the choice is how
easily a function can be specified.

## Plaza, 2026-11-16 (`scenarios/profile_plaza.toml`, 30 fps cap)
Tresoutta Plaza with its crowd, standing then pushing off, 35.5 s (1,066
frames).

**CPU per frame on the Odin 2** (run `20261116-130345_profile_plaza`,
release, per-thread CPU-time sampling):

| Thread | CPU ms/frame |
|---|---|
| main (game tick) | 2.9 |
| render (guest swap; includes the native renderer) | 2.4 |
| EA::Jobs workers (3 busy) | 4.4 |
| **total** | **~9.7**, spread over several cores |

At the 30 fps cap the guest is far from CPU-bound on the device: no thread
uses more than ~3 ms of a 33 ms frame. The main thread's busy wall time in
`frames.csv` (p50 ~7 ms) is mostly waiting for jobs. So replacing single
functions buys little frame time at 30 fps. The value of RE-7 is in
understanding and correctness (the character path), and in CPU and power.
An uncapped profile is the next measurement for 60 fps headroom.

**Largest items (Odin 2, self ms/frame unless noted):**

| ms/f | Function | Thread | Notes |
|---|---|---|---|
| 0.45 + 0.44 | `EA::Jobs::LocalBackend::RunSingleJob` (`sub_8290AFA0`) and `sub_82AE30F0` it calls | job | Time in the dispatcher itself; looks like spin/backoff while waiting for work, not work |
| 0.64 | libc (no caller frames: libc has no frame pointers) | job | Likely the same waiting (sched_yield / futex) |
| 0.30 incl | `WakeThreads` → `SetEvent` → host `NtSetEvent` | job, main | Host event signaling per job wake (Windows: 0.85 ms/f on main through `XEvent::Set`). An SDK-side fix: skip the signal when nothing waits |
| 0.33 incl | `sub_82E89B80` (+ `sub_82E88BA0`), under `sub_82D1DE08` (0.50 incl) | job | Unnamed; to identify |
| 0.24 | `DoFrustumCulling` (`sub_82816A40`, in `Sk8::VectorizedCullingJob`) | job | Pure SIMD math; a clean replacement candidate |
| 0.36 incl | collision: `sub_8277C5D8` → `Sk8ComputeContactPoints`, `ClusteredMeshCluster`, `AABBox` tests | job | Several small functions |
| 0.23 (all threads) | `rex::ppc::simde_mm_vrsqrtefp_ps` | all | The recompiler's emulation of the `vrsqrtefp` estimate, not inlined; an SDK-level candidate (bit-exact required) |
| 0.07–0.09 | `RefpackDeflate::Compress` (`sub_82A85368`) | job | Larger on Windows (0.46); probably the replay buffer |
| 0.88 | libc under the guest swap | render | Probably memcpy in the native renderer's frame end |

**Windows** (run `20261116-125910_profile_plaza`) ranks the same functions.
Its absolute numbers are higher and less exact: it samples wall time and
drops idle samples by a cycle threshold. Host code in the release exe has no
names there (no PDB); the Odin 2 binary names it.

## Next
- Profile uncapped (`skate3_guest_fps_cap = 0`) to find the critical path
  at 60 fps.
- Identify `sub_82D1DE08` / `sub_82E89B80`.
- Candidates by cost × specifiability: `DoFrustumCulling`, the
  `vrsqrtefp` helper (SDK), the job-wake signaling (SDK), RefPack
  compress.
