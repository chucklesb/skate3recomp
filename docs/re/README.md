# Reverse engineering

What we know about Skate 3's own code, learned while debugging this fork,
and how we replace game functions with native code. This is a continuous
workstream alongside the [roadmap](../roadmap.md), in this repo (decided
2026-11-15), not a separate fork.

**The goal:** every part of the fork that *guesses* what the game is doing
should eventually *know* it. Guesses are listed in
[workarounds.md](workarounds.md); each one is replaced by an RE-backed
implementation and then deleted.

| Page | What it covers |
|---|---|
| [symbols.toml](symbols.toml) | The function map: address, name, signature, role, confidence, evidence |
| [workarounds.md](workarounds.md) | Every host-side layer that guesses instead of knowing, and what would replace it |
| [types.md](types.md) | Structure layouts, from the Skate 2 PDB, with what TU3 is known to keep or change |
| [characters.md](characters.md) | Characters: skeleton, parts, palettes, the pack and bone-transform functions |

## Conventions

- **Addresses** are Title Update 3 (3.0.3.0) image addresses, written as the
  recompiler names them: `sub_827A52C8`.
- **Names** use the game's own names where they are known (from strings,
  RTTI, vtables or debug text), e.g. `Sk8::UpdateBoneTransforms`. A name we
  made up is marked `confidence = "guess"`.
- **Confidence:** `confirmed` (seen working: a hook or probe proves the role),
  `likely` (strong evidence, not proven), `guess`.
- **Evidence:** where it was learned (a file and hook, a log line, a run,
  a commit).
- **What may be published here:** names, addresses, signatures, structure
  offsets and layouts, and descriptions of behaviour. **Not** decompiled or
  disassembled game code, and not game data or assets.

## Workflow

1. **Learn while debugging.** A probe or hook shows what a function does or
   what a structure holds.
2. **Write it down the same day:** an entry in `symbols.toml`, and a note on
   the system's page (create one per system: characters, front end, reels,
   camera, ...). Code comments may point here instead of repeating it.
3. **Replace when it pays.** A native replacement goes in `src/re/` (planned),
   behind a cvar, and is verified in **shadow mode** first: run the original
   and the replacement on the same inputs over a scenario and compare every
   output. Default-on only after zero divergences on Windows and the Odin 2.
4. **Delete the guess** it makes unnecessary
   ([workarounds.md](workarounds.md)); no permanent fallback.

## Toolkit

Use these before writing a new helper; extend them instead of duplicating.
The usual order: observe live, snapshot, read the code, name it, verify.

| Step | Tool | What it does |
|---|---|---|
| Observe live | harness `mem` verb ([scenario-harness.md](../scenario-harness.md)) | Find a value in guest memory (`find`, then `refine` as it changes in game, including unknown values with `cmp`), read it, write it. Live: `rs.sh send mem op=find value=263750` |
| | `--skate3_harness_re_dump` / `re_poke` cvars (`src/harness/harness_game.cpp`) | One-shot reads (with pointer following) and writes from the command line, on the guest's main thread |
| | `--skate3_harness_re_probe`, `re_trace` (`src/harness/harness_re.cpp`) | Log the script bindings the game calls and every new message type with its sender; trace the hooks there |
| Snapshot | harness `snapshot` verb | Whole guest memory to `snapshots/<tag>.gsnap`, on Windows or the Odin 2 |
| | `tools/re/gsnap.py` | Search a snapshot (`find-u32`, `find-f32`), diff two, read, find who points at an object (`refs`). Its RTTI commands (`classof`, `owner`) find nothing in this game: Skate 2 and Skate 3 were both built without MSVC RTTI (checked 2026-11-15) |
| | `tools/re/registry.py`, `messages.py` | The game's object registry (names by hash) and the message id → `cMsg` class table, from a snapshot |
| Read the code | `tools/re/xrefs.py` | From `generated/`: callers, callees, functions that build an address (`refs`), disassembly (`dis`) |
| | `tools/re/decomp.py ADDR` | Ghidra's decompiled C (one-time project: `tools/re/ghidra/setup.py SNAP.gsnap`; output cached in `out/re/decomp/`, never committed) |
| Name it | [symbols.toml](symbols.toml), `tools/harness/re_symbols.py` | The map; `lookup`, `coverage`, `check`. `run_scenario.py symbolize` and crash summaries print map names next to `sub_` addresses |
| Verify | scenarios, char_sync, the census | A probe or scenario that shows the role; `confidence = "confirmed"` only then |

## Names from Skate 2 (roadmap RE-2)

There are no public Skate 3 symbols. A Skate 2 debug build (Xbox 360, January
2009: `sk82_na_m.xex` and a full 86 MB PDB) shares the engine and much of the
game code. It stays outside the repo (game code); its folder goes in
`SKATE3_RE_REF_DIR`.

- `tools/re/sk2_match.py prepare`: repacks the PDB (`msf_repack.py`: its old
  big-directory MSF format stops `llvm-pdbutil`), lists its 52,965 named
  functions, and unpacks the XEX with `rexglue dump-xex` (devkit key).
- `sk2_match.py match`: pairs Skate 3 functions with Skate 2 ones, by:
  - **exact:** the same normalized code (branch and address bits masked);
  - **strings:** the same string literal, used by one function in each build;
  - **callgraph:** the callees of matched pairs, by position;
  - **vtable:** slots of vtables lined up by matched anchors (the PDB names
    Skate 2's vtables);
  - **layout / layout-align:** the gaps between matched pairs, in link order.

  The Skate 2 build keeps its asserts (`xcore::FailAssert`, `DebugBreak`),
  which retail Skate 3 compiled out. The matcher strips them before hashing
  and leaves them out of call sequences.
- `sk2_match.py match --holdout 0.3`: measures precision by hiding 30% of the
  exact pairs and letting the other passes find them again.
- `tools/re/sk2_vt.py import | run | export | compare`: Ghidra's Auto Version
  Tracking (Skate 2 source, TU3 destination) in a copy of the Ghidra project
  (`out/re/ghidra-vt/`). The Skate 2 import and analysis take about 6 minutes;
  `run` takes about 20 minutes, using up to 10 GB, and prints nothing while it
  works. `compare` checks VT's accepted matches against `sk2_match.py`'s.
- `sk2_match.py export`: merges both (VT fills gaps and wins over everything
  but our exact and string pairs) and writes [names_sk2.tsv](names_sk2.tsv) (names only:
  address, name, method, confidence). `re_symbols.py lookup` and symbolize
  read it (symbolize marks these names `sk2?`); `symbols.toml` takes priority.

**2026-11-15:**
- **Coverage:** 9,326 of 47,938 functions named (19.5%): 8,020 from
  `sk2_match.py`, plus 1,306 from Version Tracking.
- **Cross-check:** where both named a function, VT agreed with our exact and
  string pairs 99-100% of the time, with callgraph and layout 93-97%, and with
  vtable and layout-align 79-85%. Those figures match the hold-out estimates.
  VT's own correlators agreed with our exact and string pairs 98-100%.
- **Types:** `tools/re/sk2_types.py show NAME [--flat]` prints a Skate 2 class
  layout (offsets, field names and types, base classes expanded with
  `--flat`). `find REGEX` searches class names; `check` validates
  [types.md](types.md) against the PDB. On the four structures we read, every
  offset our code relies on matches a named Skate 2 field.
  `cModelInstance` and `SkaterPresEntity` are unchanged where we use them;
  `MeshContext` (0x34 to 0x50) and `cLivingWorldPresEntity` grew in Skate 3.
  Board sales don't exist in Skate 2: the career pilot (RE-4) needs Skate 3's
  own code.
- **The rest is mostly code Skate 3 added or changed.** Next: BSim, or
  similarity on decompiler output.
- **Precision**, on unchanged functions:
  - exact and strings: about 100%;
  - callgraph and layout: about 93-95%;
  - vtable and layout-align: about 80-84%.
- **Treat every name as a lead:** two of our five hand-named functions that
  got a Skate 2 name got the wrong one.

## Skate 3's own names (roadmap RE-3)

Names that come from Skate 3 itself, so they reach the code the Skate 2
transfer can't. They rank above the Skate 2 names.

- **Lua bindings:** `tools/re/bindings.py scan | export | lookup`. The image
  holds 111 script-binding tables of `{0, type, "name", function}` entries
  (type 2: a function; type 3: exports an enum, e.g. `eAwards`). That names
  856 functions with the script-facing name of the C function behind them,
  for example `CompleteChallenge`, `SetSessionMarker`, `SpawnAIAtLocation`,
  in [names_bindings.tsv](names_bindings.tsv). 17 are shared stubs: debug
  bindings compiled out to one function, listed with all their names.
  Library names aren't stored as plain strings.
- **Front-end screens:** [fe_screens.tsv](fe_screens.tsv), the 86 FE screen
  ids with their names and Flash movies (`tools/re/fe_screens.py`, from the
  table at `0x8302F490`), for reading the harness's `fe` stacks and the FE
  push log (e.g. 22 CrossBar, the pause menu; 64 MilestoneUnlock).
- **Name hashes:** the game keys names by a 64-bit lookup8 hash
  (`sub_82B73AC0`); `tools/re/challenges.py hash NAME` computes it.
  [challenges.tsv](challenges.tsv) names 94 career challenges.
- **Messages:** [messages.tsv](messages.tsv), 368 `cMsg` types by id (from
  `tools/re/messages.py`), for reading the RE probe's message census.
- **Bindings as a check on the Skate 2 names:** on 82 functions named both
  ways:
  - strings and layout fit the binding name (32 of 33);
  - layout-align fit only 2 of 19, so its names are no longer exported;
  - exact matches of tiny functions matched unrelated stubs, so they're now
    `guess`.

## How a replacement works (ReXGlue)

Every recompiled game function is a C++ function `sub_XXXXXXXX(ctx, base)`
(`ctx` = the PowerPC registers, `base` = guest memory). Defining it
ourselves overrides the generated one, which stays callable as
`__imp__sub_XXXXXXXX`:

```cpp
extern "C" REX_FUNC(sub_827A52C8) {   // Sk8::UpdateBoneTransforms
  const uint32_t parts = ctx.r3.u32;  // arguments in r3, r4, ...
  __imp__sub_827A52C8(ctx, base);     // the game's own code
  // ... observe the result, or skip the call and do it natively
}
```

Calls that reach a function through a pointer or a vtable can also be
redirected at runtime: `dispatcher->SetFunction(0x826FDD70,
&Skate3DemoPath_LanguageSelectStateHook)` (`src/skate3_demo_path.cpp`).
Function boundaries the recompiler gets wrong are fixed in
`config/skate3_functions.toml` and `config/skate3_tu_functions.toml`.
About 144 game functions are referenced from `src/` today;
`tools/harness/re_symbols.py coverage` lists those not in the map yet.

## References

- **The SDK source:** `third_party/rexglue-sdk` (our fork,
  `chucklesb/rexglue-skate3`). Its own docs are thin; the source is the
  reference. PowerPC and VMX128 instruction references:
  `third_party/rexglue-sdk/docs/ppc/`.
- **The generated code:** `generated/` (from the XEX and TU3; not committed).
  A function's body there is its exact behaviour.
- **A disassembler project** (Ghidra or IDA) is useful, but it holds game
  code: keep it outside the repo.
