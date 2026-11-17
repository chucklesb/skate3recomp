# Native replacements (RE-7 catalog)

Every guest function we have specified, and whether native code runs in
its place. RE-7's goal is native code we understand in place of rexglue's
recompiled code, whether or not it saves cycles
([roadmap.md](../roadmap.md), [hot-functions.md](hot-functions.md) for
cost).

**The rule:** when a function is specified (a [symbols.toml](symbols.toml)
entry with its inputs, outputs and side effects), either replace it the
same day (trivial: getters, wrappers, simple loops) or add a `specified`
row here saying what's missing.

**Statuses:**
- `native-on`: native code runs in its place by default;
- `native-shadow`: native code runs and is compared, the recompiled code
  still runs;
- `specified`: understood but not replaced (the reason is in the row);
- `hooked-only`: a hook observes or adjusts it; the recompiled code runs.

**Kinds and how they're verified** (the framework is
`src/native/skate3_native_replace.{h,cpp}`, `replace:` lines,
`tools/harness/replace_report.py`):
- `pure`: computes guest memory from guest memory. A Compute family:
  shadow mode compares every written byte; `on` only once shadow is 100%
  exact everywhere.
- `control`: loops, wrappers, calls into other guest functions. A Body
  family (`native_replace::Family` with a `body`, off/on only; the global
  `skate3_native_replace_all = off|shadow` runs the recompiled code): no
  shadow (nothing to compare), so the native body follows the disassembly
  call for call (same guest calls and arguments, the same stack frame size,
  LR set to the original return addresses, the same r3) and is verified by
  runs (scheduling counters, char_sync, frame times).

## Catalog

| Address | Name | Kind | Status | Where / verified by | Next |
|---|---|---|---|---|---|
| `sub_827A52C8` | `Sk8::UpdateBoneTransforms` | pure | native-on | `skate3_native_pose.cpp`; shadow bit-exact on both platforms | - |
| `sub_827E5B30` | `PackAndMultiplyMatricesForUpload` | pure | native-on | `skate3_native_pose.cpp`; shadow bit-exact | - |
| `sub_827C1D38` | LivingWorld batch pack writer | pure | native-on | `skate3_native_pose.cpp`; shadow bit-exact | - |
| `sub_82783D68` | `Sk8::SkaterPresEntity::BindConstants` | control | specified | register map in [characters.md](characters.md); the renderer reads the rows from the entity (`char_rows_entity`) | Replace: it calls `SetParameter` with entity pointers (the map), then the parent |
| `sub_827C1720` | `cLivingWorldPresEntity::BindConstants` | control | specified | as above | as above |
| `sub_827C25D8` | LivingWorld-derived BindConstants | control | specified | as above; `sub_827816C0` copies the 9 rows | Replace: parent BindConstants, the row copy, then `SetParameter` |
| `sub_827816C0` | LivingWorld lighting row copy | pure | native-on | `skate3_native_pose.cpp` `lwrows` (`skate3_native_lw_rows_replace`); a 160-byte copy (9 vectors, 4 words); shadow bit-exact on Windows (plaza 4,839 calls, forearm reel 621); on, Odin 2: plaza 4,894 and reel 825 calls, reel char_sync 0 | - |
| `sub_82785260`, `sub_82793F70`, `sub_82785528` | Colorized / CAC / skater-layout BindConstants | control | hooked-only | class tags only | Specify (paint/colorize rows) |
| `sub_82B76080` | D3D wait poll (one turn of `BlockOnFence`) | control | hooked-only | `skate3_native_fence.cpp`: blocks on the GPU write-back signal before the guest poll | Replace the poll body (spin hints, fence compare, deadlock check) |
| `sub_82B755C0` | `D3D::CDevice::BlockOnFence` | control | specified | loops on `sub_82B76080` | Replace with the poll |
| `sub_8290AFA0` | `EA::Jobs::LocalBackend::RunSingleJob` | control | hooked-only | `skate3_native_jobs.cpp` (arming, stats) | Specify the slot layout (8-byte state words at backend+8, count +16, 656-byte job records at backend+4) and the CAS claim; the claim has no shadow, so it needs a careful run-based check |
| `sub_8290B750` | EA::Jobs worker loop | control | native-on | `skate3_native_jobs.cpp` `j_worker` (`skate3_native_jobs_replace`); jobs: counters level, reel char_sync 0 | - |
| `sub_8290B320` | `WakeThreads` | control | native-on | `j_wake`: both paths native; signals only armed workers when `skate3_native_job_wake = on` | - |
| `sub_82909738`, `sub_82909740` | `Get/SetJobThreadSleepTimeoutMS` | control | native-on | `j_gettmo`, `j_settmo` (backend+1592) | - |
| `sub_82EDFEC0` | `WaitForSingleObject` | control | native-on | `j_wait` (all 105 callers) | - |
| `sub_82EE7880` | `WaitForSingleObjectEx` | control | native-on | `j_waitex`: the timeout helper `sub_82EEEA98`, the `NtWaitForSingleObjectEx` import (retried on status 257 when alertable), `sub_82EE6260` on failure; Windows and Odin 2 runs (Odin 2 plaza: 790,594 waits native, jobs late 7 of 5,921) | - |
| `sub_82EEEA98` | ms to a kernel timeout | control | native-on | `k_timeout`: -1 gives 0 (INFINITE), else stores u32(ms) * -10000 at r3 | - |
| `sub_82EE6260` | set the last error from an NTSTATUS | control | native-on | `k_lasterr`: `RtlNtStatusToDosError`, stored at `*(r13+256)+352` unless `*(r13+336)` is set (32 callers) | - |
| `sub_824741F0` | spin-wait on a job | control | native-on | `j_spin` | - |
| `sub_82608E38` | boot logo `OnUpdate` (FE state 5) | control | native-on | `skate3_native_boot.cpp` `boot_logo` (`skate3_native_boot_replace`): `+60 += dt`, done byte `+64` at 4000 ms or on the intro skip; [boot.md](boot.md) | - |
| `sub_82609050`, `sub_82609238` | title `OnUpdate` / `ReceiveInput` (FE 22) | control | hooked-only | the quick title wraps them (`skate3_title_quick_start`); [boot.md](boot.md#the-title-screen-fe-state-22) | Replace with the boot handler chain |
| `sub_82639400` | language select `OnUpdate` (FE 47) | control | native-on | `skate3_native_boot.cpp` `lang_update`: `+60 += dt`, at 10,000 ms reset and pop itself (`sub_82D0AC88` on the FE manager); verified 2026-11-17 by `boot_timing` (10.57 s, Windows and the Odin 2) | - |
| `sub_82608E00`, `E58`, `E70`, `E88`, `sub_826098B0`, `sub_8260A270`, `sub_82634BB0`, `sub_825EF0F8`, `sub_825EC7E8`, `sub_8260FD88`, `sub_82638608`, `sub_82638E00`, `sub_826393E8` | OnEnter (vtable +0x0C) of FE states 5-9, 11, 28, 32, 41, 43, 44, 47, 70 | control | native-on | `fe_open` (one family, each wrapper passes its N): tail call `this->OpenScreen(N, 0)` (vtable +56), N = 5-10, 23, 60, 61, 68, 85, 86, 94. Found by matching the exact code in every FE state's vtable. Exercised by runs: states 5 (logo) and 47 (language); the rest are the same six instructions | - |
| `sub_82608E18` | OnLeave (vtable +0x18) of FE states 5-8 | control | native-on | `fe_close`: done byte `+64 = 1`, tail call `this->CloseScreen(0)` (vtable +60) | - |
| `sub_826504C8` | OnLeave of FE states 10, 11, 13 | control | native-on | `fe_closescreen`: tail call `this->CloseScreen(0)` | - |
| `sub_82608F18`, `sub_82608F58` | title OnEnter / OnLeave (FE 22) | control | native-on | `title_enter`: resets +76/+52/+64/+80, opens screen 2 unless mode +68; `title_leave`: the leave sound (`sub_825DFB88`, hash `0xE3071166217C9661`), then `CloseScreen(0)`; `boot_skip`, `title_quick_start` | - |
| `sub_826FDC50`..`sub_826FE1D8` | `StateBootFlow` handlers (7) | control | specified | [boot.md](boot.md): events, transitions, the load phase | Native chain: needs the movie handler's side effects understood |
| `sub_8296D1F0`, `sub_8296D0D0`, `sub_826991B8` | pad ring pop / push / unpush (Sk8AppMan) | control | native-on | `skate3_native_padring.cpp` `ring_pop`/`ring_push`/`ring_unpush` (`skate3_native_padring_replace`); host atomics on the guest indices; Windows + Odin 2: plaza frame times level, reel char_sync 0, boot_skip input; [boot.md](boot.md#before-the-boot-flow-the-game-tick-and-its-pad-sample-rings) | - |
| `sub_82699230` | pad ring consume | control | specified | drains ring idx, copies the 4 pad records per sample (`sub_82966F30`) | Replace with `sub_82966F30` (the per-pad debounce, specify it) |
| `sub_826D34B8` | Sk8AppMan per-tick update | control | hooked-only | harness request point, gate probe | Large (every game system's update); later |
| `sub_829661F0` | render loop | control | specified | stopwatch, app mutex, presenter render, unlock | Replace once the presenter's render path is mapped |
| `sub_82816A40` | `DoFrustumCulling` | pure | native-on | `skate3_native_cull.cpp` `cull`; shadow bit-exact on both platforms; [culling.md](culling.md) | - |
| `sub_82817138` | `Sk8::VectorizedCullingJob` | control | native (with `cull` on) | `cull_job` | - |
| `sub_82812A20` | `BufferedIterativeJobEntryPoint<VectorizedCullingJob>` | control | native (with `cull` on) | `cull_ent` | - |

Hooks that exist for the harness, the native renderer or the demo path
(about 115 `REX_FUNC` overrides in `src/`; list them with
`grep -rh 'extern "C" REX_FUNC' src`) observe or redirect game state and are
not replacement candidates until their function is specified; the ones in
`symbols.toml` have a `hooked` field.
