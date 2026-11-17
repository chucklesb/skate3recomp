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
  family: no shadow (nothing to compare), so the native body follows the
  disassembly call for call and is verified by runs (scheduling counters,
  char_sync, frame times).

## Catalog

| Address | Name | Kind | Status | Where / verified by | Next |
|---|---|---|---|---|---|
| `sub_827A52C8` | `Sk8::UpdateBoneTransforms` | pure | native-on | `skate3_native_pose.cpp`; shadow bit-exact on both platforms | - |
| `sub_827E5B30` | `PackAndMultiplyMatricesForUpload` | pure | native-on | `skate3_native_pose.cpp`; shadow bit-exact | - |
| `sub_827C1D38` | LivingWorld batch pack writer | pure | native-on | `skate3_native_pose.cpp`; shadow bit-exact | - |
| `sub_82783D68` | `Sk8::SkaterPresEntity::BindConstants` | control | specified | register map in [characters.md](characters.md); the renderer reads the rows from the entity (`char_rows_entity`) | Replace: it calls `SetParameter` with entity pointers (the map), then the parent |
| `sub_827C1720` | `cLivingWorldPresEntity::BindConstants` | control | specified | as above | as above |
| `sub_827C25D8` | LivingWorld-derived BindConstants | control | specified | as above; `sub_827816C0` computes the 9 rows | Specify `sub_827816C0` (pure: a Compute family) |
| `sub_82785260`, `sub_82793F70`, `sub_82785528` | Colorized / CAC / skater-layout BindConstants | control | hooked-only | class tags only | Specify (paint/colorize rows) |
| `sub_82B76080` | D3D wait poll (one turn of `BlockOnFence`) | control | hooked-only | `skate3_native_fence.cpp`: blocks on the GPU write-back signal before the guest poll | Replace the poll body (spin hints, fence compare, deadlock check) |
| `sub_82B755C0` | `D3D::CDevice::BlockOnFence` | control | specified | loops on `sub_82B76080` | Replace with the poll |
| `sub_8290AFA0` | `EA::Jobs::LocalBackend::RunSingleJob` | control | hooked-only | `skate3_native_jobs.cpp` (arming, stats) | Specify the slot layout (8-byte state words at backend+8, count +16, 656-byte job records at backend+4) and the CAS claim; the claim has no shadow, so it needs a careful run-based check |
| `sub_8290B750` | EA::Jobs worker loop | control | hooked-only | `skate3_native_jobs.cpp` (registers the worker) | Native body |
| `sub_8290B320` | `WakeThreads` | control | hooked-only (signal path native when `skate3_native_job_wake = on`) | `skate3_native_jobs.cpp`; `jobs:` late/hit counters | Native body for both paths |
| `sub_82909738`, `sub_82909740` | `Get/SetJobThreadSleepTimeoutMS` | control | specified | backend+1592 | Native body (trivial) |
| `sub_82EDFEC0` | `WaitForSingleObject` | control | hooked-only | `skate3_native_jobs.cpp` (stats) | Native body (trivial) |
| `sub_824741F0` | spin-wait on a job | control | specified | caller histogram in `jobs:` | Native body |
| `sub_82816A40` | `DoFrustumCulling` | pure | specified (in progress) | 0.24 ms/frame on the Odin 2 job threads | Compute family, shadow then on |
| `sub_82817138` | `Sk8::VectorizedCullingJob` | control | specified (in progress) | calls `DoFrustumCulling` | Native body |
| `sub_82812A20` | `BufferedIterativeJobEntryPoint<VectorizedCullingJob>` | control | specified (in progress) | calls the job | Native body |

Hooks that exist for the harness, the native renderer or the demo path
(about 115 `REX_FUNC` overrides in `src/`; list them with
`grep -rh 'extern "C" REX_FUNC' src`) observe or redirect game state and are
not replacement candidates until their function is specified; the ones in
`symbols.toml` have a `hooked` field.
