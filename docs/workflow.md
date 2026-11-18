# Workflow

How every investigation, fix, change or feature is done, so that what we
learn is never thrown away and every fix moves the game toward our own code
([plan.md](plan.md)). Follow it every time; the doc check (step 8) enforces
the parts a script can see.

## 1. Reproduce

- Write or reuse a scenario ([scenario-harness.md](scenario-harness.md)).
  Prefer a skate.Reel replay over input playback; for a one-frame bug,
  record every frame.
- A known issue gets its entry in [known-issues.md](known-issues.md),
  starting with what you see on screen.

## 2. Understand

- Find the game code involved with the RE toolkit
  ([re/README.md](re/README.md#toolkit)): names, callers, the layout, a
  probe or a memory snapshot.
- Find *why* the bug happens, not just where. A cause is confirmed when a
  change to it changes the picture (an A/B run), not before.

## 3. Record it the same day

- Every function named or understood: an entry in
  [re/symbols.toml](re/symbols.toml) (name, signature, role, confidence,
  evidence). Then `python3 tools/harness/re_symbols.py check`.
- Every structure: [re/types.md](re/types.md) and its C++ type with
  `static_assert`ed offsets.
- The system: a note on its subsystem page ([re/README.md](re/README.md)
  lists them).
- A known issue: its root cause in the KI entry.
- Never commit decompiled code, disassembly, or game files.

## 4. Fix it in native code

- **Default:** replace the code the fix touches (the function, or its
  class) with our own implementation under the standard in
  [re/replacements.md](re/replacements.md): named structs, behaviour in
  ordinary C++, registers only at the call boundary, class by class. Add
  its catalog row.
- **A workaround** (a guess about the game's state, a fallback) only when a
  native fix isn't possible yet: it ships **default-off** as a comparison
  switch, with a row in [re/workarounds.md](re/workarounds.md) saying what
  would replace it.
- Limits (array sizes, counts) come from the game's own layout, never from
  what one test happened to show.

## 5. Verify

- A Compute replacement: shadow mode (every output byte compared), then on.
- A Body replacement: the scenarios that reach it, with the replacement on
  and off: same screenshots, the family's `bad` (refused) count 0. A body no
  scenario reaches is marked "not reached" in its catalog row.
- Windows first, then the Odin 2 (the reference device). Compare the
  `replace:` counters against the previous build on both.
- Frame-time claims: medians of 3 alternating runs.

## 6. Check the subsystem's coverage

```
python3 tools/re/coverage.py --subsystem NAME [--counts RUN]
```

lists what in the touched subsystem is still recompiled (most called first
with a call-count run). Decide with the user: fill the gaps now, or note
them under the subsystem page's **Next**. Regenerate the report:
`python3 tools/re/coverage.py --write [--counts RUN]`.

## 7. Update the trackers

- The KI entry's status (fixed: the root cause and what became native).
- The RE item's header block (Status, Next) on its page and its row in the
  hub ([re/README.md](re/README.md#the-re-items)).
- The roadmap's Status line ([roadmap.md](roadmap.md)), and the backlog
  ([open-work.md](open-work.md)) if something was added or finished.
- [re/coverage.md](re/coverage.md) (step 6).

## 8. Commit

- `python3 tools/re/check_docs.py --staged` runs as the git pre-commit hook
  (install once with `tools/re/install-hooks.sh`). Errors block the commit: a
  native body without its map entry or catalog row, an undefined RE id, an
  RE page without its header block, a broken link. Fix them; skip only in
  an emergency (`SKATE3_SKIP_DOC_CHECK=1`, which is logged).
- Commit messages say what changed and how it was verified.
- Push only when asked.
