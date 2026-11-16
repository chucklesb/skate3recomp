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
