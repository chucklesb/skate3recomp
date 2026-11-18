---
name: re
description: Reverse-engineer Skate 3 (TU3) code and data in skate3recomp - look up a function's name (hand map, Skate 2 transfer), print a structure layout from the Skate 2 PDB, read callers/callees/disassembly/decompiled C, find a game value in live memory and change it, take and search memory snapshots, and record what was learned. Use when asked what a game function or structure is, to find where the game keeps some state, to name or document game code, or for roadmap RE-1..RE-9 work.
argument-hint: "lookup <name|addr> | type <Class> | xrefs <addr> | decomp <addr> | find <value> | snapshot | record"
---

# Reverse engineering Skate 3

Reference: `docs/re/README.md` (the RE items, conventions, the Toolkit
table, the Skate 2 section); goals in `docs/plan.md`; every investigation
follows the `workflow` skill (`docs/workflow.md`). Arguments: `$ARGUMENTS`.

Run commands from the repo root in the project's shell
(`docs/dev-setup.md`). Live-game commands go through the harness:
`tools/harness/rs.sh send ...` (the device), or with `--target windows`.

## Which tool answers what

| Question | Command |
|---|---|
| What is `sub_X`? | `python3 -I tools/harness/re_symbols.py lookup X`, in priority order: the hand map `docs/re/symbols.toml`, then Skate 3's own Lua binding names (`docs/re/names_bindings.tsv`, `tools/re/bindings.py lookup`), then the Skate 2 names in `docs/re/names_sk2.tsv` (with method and confidence) |
| Which C function a script call runs | `python3 -I tools/re/bindings.py lookup NAME` (e.g. `CompleteChallenge`); message ids: `docs/re/messages.tsv` |
| Where is function NAME? | `re_symbols.py lookup NAME` (substring); `python3 -I tools/re/sk2_match.py lookup NAME` (with the Skate 2 address) |
| A structure's layout | `python3 -I tools/re/sk2_types.py show Class [--flat]`; `find REGEX`. The layout is Skate 2's: check TU3 before trusting an offset (`docs/re/types.md` keeps per-field status) |
| Who calls X / what X calls / its code | `python3 -I tools/re/xrefs.py callers X`, `callees X`, `refs 0xADDR`, `dis X` (needs `xrefs.py index` once) |
| Decompiled C | `python3 -I tools/re/decomp.py 0xX` (Ghidra; cached in `out/re/decomp/`; never commit the output) |
| Skate 2's version of a function | Ghidra project `out/re/ghidra-vt/project` (program `/sk2.bin`, functions `sk2_<addr>`); the address comes from `sk2_match.py lookup` |
| Where the game keeps a value | live `mem`: `send mem op=find value=N` then `op=refine value=M` after it changes in game. Unknown value: `op=find range='["0x40000000","0x50000000"]'` (no value), then `op=refine cmp=increased|decreased|changed|unchanged` |
| Career state (board sales, milestones) | `send career board_sales=N` (live) or `do = "career"` in a scenario; add `notify=true show=true` to cross milestones and get their screens with no challenge (`scenarios/career_milestone.toml`). Model: `docs/re/career.md` |
| A 64-bit key in the code (like `0xA5970491380FF1BE`) | `python3 -I tools/re/attrib.py lookup HASH` (vault class/record names, `docs/re/attrib_names.tsv`); `tools/re/challenges.py hash NAME` hashes a name (lookup8, `sub_82B73AC0`); `challenges.py show SNAP` names a career snapshot's challenges and locked actions |
| The game's data files | `python3 -I tools/re/big.py list\|extract\|grep data/big/X.big` (EB v3 + RefPack; extracts to `out/re/big/`, never commit). Databases are Attrib vaults; scripts are compiled Lua in `db/*.bin` |
| Complete a challenge without playing | `send career complete=NAME level=owned\|killed pay=true`, then `send results` (walks the results, waits for the save); `scenarios/career_complete_challenge.toml` |
| What FE screen id N is | `python3 -I tools/re/fe_screens.py show N` (table in `docs/re/fe_screens.tsv`); who pushes it: grep `generated/` for `li r4,N` before `bl 0x82d0abe0`/`ab30`/`aef0`, or the FE push log |
| Read or poke a word | `send mem op=read addr=0x...`, `op=write addr=0x... value=N` (`type` u8/u16/u32/f32/f64) |
| Whole memory | `send snapshot tag=x`, then `rs.sh fetch`; search with `python3 -I tools/re/gsnap.py find-u32 SNAP V`, `diff`, `refs` |
| Script bindings and messages | the RE probe `--skate3_harness_re_probe=true` (logs Lua bindings called and new message types); `tools/re/messages.py SNAP` (message id table) |
| Names in a crash or address list | `rs.sh symbolize <addrs>` prints map names; Skate 2 names are marked `sk2?` |
| A character's pose / palettes from the source | the skeleton store, `skate3_native_render_scene_skeleton=shadow\|on` (`docs/re/characters.md`); `skel_shadow_report.py`; for frames after a jump, `skate3_native_render_scene_skeleton_log_jumps=true` (`skel-jf`/`skel-new`/`skel-draw` lines) |
| A guest mesh in an emulated RenderDoc capture (no labels) | `qrenderdoc.exe --python tools/harness/rdc_fetch_scan.py` with `RDC_SCAN_*` env (finds draws by vertex buffer address; slow, ~15 draws/s). Ask first whether a memory probe answers it faster (KI-125: `ropa-mem:`) |
| The boot sequence (RE-8) | `docs/re/boot.md`: the StateBootFlow handler chain, FE state ids vs screen ids, timers, the load phase. `scenarios/boot_timing.toml` + `tools/harness/boot_timing.py`; `skate3_demo_path_probe` logs `Skate 3 boot:` lines and the FE state table; `tools/re/vtable.py` for a state's functions |
| The menus and HUD (RE-9) | `docs/re/ui.md`: APT movies per FE screen (`fe_screens.py`, `big.py extract data/big/fedata.big`; `strings` on a `.apt`/`.const` lists its natives and parts), FE states, the Options tables, popups, the localizer hook (`src/skate3_options_menu.cpp`). Open any FE state without navigating: the harness `fe` step (`op = "push" state = N`, `op = "pop"`; `fe_states_smoke.toml`). Input readiness: `fe_input` events / `fe_ready`. Small FE-state methods: find them by matching code across every state's vtable (the probe's FE state table dump) |
| A class's methods, and which are small leaves | `python3 tools/re/leaves.py vtable VTADDR` (each slot: name, and for leaves what they do, "u32[r3+72] = r5"); `lookup X` (+ the data words pointing at it); `list --kind store`. A FINDER only: it never generates code |
| Which functions the game calls most (what to RE next) | the call-count build: `tools/cross-windows/build.sh callcount build stage` (stages into `SKATE3_WINDOWS_STAGE_ROOT` from `.claude/local.env`), `tools/harness/rs.sh windows scenarios/callcount_plaza.toml --config callcount --no-build`, then `python3 tools/re/leaves.py rank <run>` (functions, then vtables by summed calls). Steps `do = "callcount" op = "reset"/"dump"` in any scenario. 7 min build, still 30 fps |
| Can a recompiled function be replaced natively (RE-7)? | Only as our own implementation (docs/re/replacements.md "The standard"): a named struct (`rex::be<T>` fields, `static_assert` offsets, e.g. `src/native/skate3_fe_types.h`), the behaviour in plain C++, registers only for arguments/returns/unreplaced guest calls, a symbols.toml entry. NEVER re-emit rexglue's statements or mirror scratch registers (the 5,600-stub layer was removed for that, 2026-11-17). See the next row |
| (replacing, continued) | Do it even when it saves no cycles: a specified (understood, named) function is replaced the same day, anything else gets a `specified` row in `docs/re/replacements.md` (the catalog). Pure functions (guest memory in, guest memory out) are Compute families on `src/native/skate3_native_replace.h`: shadow until 100% bit-exact, then on. Loops/wrappers are Body families (off/on, no shadow): follow the disassembly call for call, verify by runs. Recipe for pure code: `xrefs.py dis` (Ghidra fails on VMX128), copy the guest's op order, use the recompiler's simde primitives under `ctx.fpscr.enableFlushMode()`, mask lvx/stvx addresses, direct guest reads (not GuestTryCopy) |

## Traps

- **Skate 2 names are leads, not facts.** Measured precision: exact and
  strings ~100%, callgraph and layout ~93-97%, vtable and layout-align ~80-85%
  (`sk2_match.py match --holdout 0.3`). Two of five hand-named functions got a
  wrong Skate 2 name. Confirm with a probe before relying on one.
- **No RTTI** in either game: `gsnap.py classof` and `owner` find nothing.
- **Layouts changed:** `MeshContext` 0x34 → 0x50, `cLivingWorldPresEntity` grew.
  Board sales don't exist in Skate 2 at all.
- **`xrefs.py refs` finds only `lis`/`addi` pairs.** A string with 0 refs is
  often reached through a pointer table: search `out/re/ghidra/skate3_tu3.bin`
  for its address as a big-endian u32 (that's how the FE screen table was
  found). Virtual functions have no callers either: find the vtable the same
  way, or hook the function and log `r3`.
- **Career changes are kept only after the game saves:** walk the results
  to the end (`results` step; New Teammate's default opens Create-a-Skater)
  and wait for the save (the step watches `SKATER.P`). Quitting during the
  save loses it. Compare across runs with before AND after values.
- **A state write alone may do nothing visible.** The game reacts on its own
  code paths (career milestones needed the unlock check plus the results
  presenter's flag), so look for what runs after the game's own write.
- **FE push ids are FE *state* ids**, the index into the FrontEndManager's
  state table (`*(mgr+8)`), not the screen ids of `fe_screens.tsv`; 47 and
  22 coincide, 5 and 24 don't (`docs/re/boot.md`). Get a state's
  functions from the probe's state-table dump plus `tools/re/vtable.py`.
- **Silent long jobs:** Ghidra VT (`tools/re/sk2_vt.py run`) runs ~20 min,
  ~10 GB, with no output. Say so up front; check CPU/RSS with `ps` if asked.
- **After editing a file from Windows,** `sleep 1` before WSL runs it (stale
  reads).
- **Python patch heredocs eat backslashes** (`\b`, `\n`, `C:\U...`): use the
  Edit tool for regexes, escapes and Windows paths.
- **One-frame bugs:** periodic shots miss them. Record every frame
  (`play_reel video = true`), native and emulated, and log from a point every
  built frame passes; the smoothing block is skipped on camera cuts. Rule out
  checks that are true by construction (KI-125, 2026-11-16).
- **Snapshots on the device** land in its run folder; delete big ones there
  after fetching (`adb shell rm .../run/snapshots/*.gsnap`, needs root; or
  from a DroidDeck session).

## Record it the same day

Part of the `workflow` skill (docs/workflow.md, step 3): use it for any
investigation or fix, not just this list.

1. **The name:** `docs/re/symbols.toml`, with `confidence` and `evidence`
   (probe, run, commit). Then `re_symbols.py check`.
2. **The system:** a note on its page (`docs/re/<system>.md`, created as
   needed); a structure goes in `docs/re/types.md` (then `sk2_types.py check`).
3. **A guess replaced:** update `docs/re/workarounds.md`.
4. **The tracker:** tick the RE item in `docs/roadmap.md` in the same commit.

**Never commit** decompiled code, disassembly, or anything from the Skate 2
build except names and layouts. The reference files stay outside the repo
(`SKATE3_RE_REF_DIR`, set in `.claude/local.env`).
