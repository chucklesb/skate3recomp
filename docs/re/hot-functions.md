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
| 0.45 + 0.44 | `EA::Jobs::LocalBackend::RunSingleJob` (`sub_8290AFA0`) and `sub_82AE30F0` it calls | job | RunSingleJob's self time is the slot scan, half of it finding nothing (see the job system below). `sub_82AE30F0` is a VMX job body (vector work with prefetches), not a spin |
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

## Plaza uncapped, 2026-11-16 (`scenarios/profile_plaza_uncapped.toml`)
Odin 2, run `20261116-133617_profile_plaza_uncapped`: the profile window ran
1,066 frames in 9.1 s, **~117 fps** (the run's overall 299 fps counts loads
and menus). CPU per frame (~8.5 ms frames):

| Thread | CPU ms/frame | Largest item |
|---|---|---|
| main | 0.78 | libc, job dispatch |
| render | 2.06 | **`D3D::CDevice::BlockOnFence` (`sub_82B755C0`) 0.80 ms incl, 39% of the thread**: the guest spins on the GPU fence (`sub_82B76080` 0.34 ms self, calling `GetCurrentThreadId` each turn); the native renderer (`BuildFrameScene`) 0.26 ms |
| jobs | 1.31 | `DoFrustumCulling` 0.23 ms; job dispatcher self + `sub_82AE30F0` (a VMX job body) 0.22 ms |

Even at ~117 fps no thread is CPU-bound (the busiest uses ~25% of a frame),
so the limit is the GPU or pacing. **Busy-waiting is the largest CPU cost**:
the fence spin on the render thread and the job system's spin/backoff. On a
handheld that is battery and heat for nothing. Replacing the fence wait with
a blocking wait on the host (the fence is ours: the native renderer and the
emulated GPU know when it signals) is the top battery target; the job
system's idle spin is the second.

## The fence wait (2026-11-16, `src/native/skate3_native_fence.cpp`)
`BlockOnFence` loops on `sub_82B76080`, one poll: ~32 `db16cyc` spin hints,
a read of the GPU's completed-fence word `*(device+10896)` against the value
seen last poll (`state+8`; every caller's state is `{device, ?, seen,
timestamps}`), and `DebugGpuDeadlock` after a long stall. The word is a GPU
write-back in the physical mirror (e.g. `0xFFCA3000`; on Windows native
reads must add the 0x1000 host offset, as `REX_LOAD_U32` does).

`skate3_native_fence_wait = on` (the default since 2026-11-16): the poll hook blocks on the SDK's GPU
write-back signal (`rex::graphics::WaitGpuWriteback`, raised after every
command-processor write to guest memory) while the word has not moved,
2 ms timeout, then runs the original poll. `fence:` lines (`missed` =
timeouts after which the word had moved: 0-3 per run, so the signal
covers the fence writes).

| Plaza uncapped | Render CPU ms/frame | fps | 1% low | p99 |
|---|---|---|---|---|
| Windows, off (3 runs) | 6.05 / 6.05 / 6.06 | 164.6-164.8 | 64-66 | 13.6-13.7 ms |
| Windows, on (3 runs) | ~5.1 / 5.25 / 5.50 | 164.6-164.7 | 64-65 | 12.5-13.6 ms |
| Odin 2, off | 2.05 / 2.06 | 286-291 | 59-60 | 13.8-14.1 ms |
| Odin 2, on | 1.63 | 334 | 65 | 12.5 ms |

At the 30 fps cap the guest almost never waits on a fence (1-3 polls per
run): the saving is above 30 fps.

## The job system's wake-ups (2026-11-16, `src/native/skate3_native_jobs.cpp`)
How the EA::Jobs workers find work (names in [symbols.toml](symbols.toml)):
each worker loops on `RunSingleJob` (scan the job slots, claim one with a
CAS); when nothing is runnable it waits on its own event with a 1 ms timeout
(`GetJobThreadSleepTimeoutMS`). `WakeThreads` signals every active worker's
event after work is added. There is no spin in the workers: the cost is
wake-ups that find nothing.

Measured with `skate3_native_job_stats` (`jobs:` lines; plaza scenario at
the 30 fps cap, averaged over the whole run including boot and loads; the
plaza itself runs about twice these rates):

| Per second | Windows | Odin 2 |
|---|---|---|
| `WakeThreads` calls | ~17,500 | ~17,300 |
| worker wakes by signal / by timeout | ~6,800 / ~1,470 | ~4,000 / ~1,120 |
| signalled wakes that find a job | 31% | 32% |
| timed-out wakes that find a job | ~1% | ~0.1% |
| other threads' `RunSingleJob` calls that find a job | ~12% | |

`skate3_native_job_wake = on` (the default since 2026-11-16): a worker is
armed from just before each scan until a waker consumes it, and
`WakeThreads` signals only armed workers, once per arming (ordering argument
in `skate3_native_jobs.h`). It skips ~3.2 of every 4.1 million event
signals. Timed-out wakes that then found a job (`late`, a lost wake would
show here) stayed level: Odin 2 114-134 per run on, 123-168 off.

| Plaza, medians of 3 alternating | Job threads CPU ms/frame | 1% low | busy p99 |
|---|---|---|---|
| Odin 2 30 fps, off | 4.40 | 27.1-28.0 | 11.7-13.2 ms |
| Odin 2 30 fps, on | 4.14 (-6%) | 27.8-28.3 | 11.6-12.4 ms |
| Odin 2 uncapped (2 runs), off / on | 1.40-1.45 / 1.40-1.42 | 66-74 / 69-70 | |
| Windows 30 fps (4 runs), off / on | 9.95 / 9.2 (-7%) | 29.3 / 29.3 | |

Left: two thirds of signalled wakes still find nothing, because every armed
worker is signalled and one takes the job. Waking one worker at a time
would need the jobs' thread masks to avoid stranding work on a worker that
cannot run it. The waits that run jobs on other threads are small on the
Odin 2: the spin-wait `sub_824741F0` (no sleep; masks 1 and 4) is 0.10
ms/frame on the main thread including the jobs it runs, and
`JobInstanceHandle::WaitOn` sleeps between tries.

## Culling (2026-11-16)
`DoFrustumCulling` and its two job wrappers run natively by default
(`skate3_native_cull_replace`, [culling.md](culling.md)): bit-exact in shadow,
job threads -5% CPU on the Odin 2 and -8% on Windows at 30 fps.

## Next
- Identify `sub_82D1DE08` / `sub_82E89B80`.
- Candidates by cost × specifiability: `DoFrustumCulling`, the
  `vrsqrtefp` helper (SDK), the job-wake signaling (SDK), RefPack
  compress.
