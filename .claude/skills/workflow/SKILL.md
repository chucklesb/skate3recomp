---
name: workflow
description: The skate3recomp workflow for EVERY investigation, bug fix, change or feature - reproduce, reverse engineer, record findings the same day, fix with native code instead of a workaround, verify on Windows then the DroidDeck device, check the touched subsystem's coverage, update the trackers, commit through the doc check. Use at the start of any investigation or fix, and again before every commit.
argument-hint: "start <what> | commit"
---

# The workflow

The source of truth is `docs/workflow.md` (committed); the goals behind it
are in `docs/plan.md`. Read both if they aren't in context. This skill walks
the steps and makes sure none is skipped. Arguments: `$ARGUMENTS`.

Run commands from the repo root in the project's shell
(`docs/dev-setup.md`).

## At the start (`start <what>`)

1. **Say which item and subsystem this is:** the KI (`docs/known-issues.md`),
   the RE item (`docs/re/README.md`, "The RE items") and the subsystem
   (`docs/re/subsystems.toml`). Read that subsystem's page header (Goal /
   Done when / Status / Next).
2. **Reproduce** with a scenario (the `harness` skill); a reel over input
   playback.
3. **Understand** with the `re` skill's tools. Name the cause only after an
   A/B changes the picture.
4. **Record as you learn,** not at the end: `symbols.toml`, `types.md` and
   the C++ type, the subsystem page, the KI's root cause.
5. **Fix natively** under the standard (`docs/re/replacements.md`): named
   structs, behaviour in plain C++, class by class, a catalog row. A
   workaround only default-off with a `docs/re/workarounds.md` row; say so
   to the user and why a native fix isn't possible yet.
6. **Verify:** shadow (Compute) or on/off scenarios (Body), `bad` 0, same
   shots; Windows first, then the device; compare `replace:` counters with
   the previous build on both. Mark any body no scenario reaches.
7. **Coverage:** `python3 tools/re/coverage.py --subsystem NAME [--counts RUN]`.
   Show the user what's left in the subsystem and ask: fill the gaps now,
   or note them under the page's **Next**.

## Before every commit (`commit`)

1. Trackers: the KI status, the RE item's header block and hub row, the
   roadmap Status line, `open-work.md` if the backlog changed.
2. `python3 tools/re/coverage.py --write [--counts RUN]` when anything
   became native.
3. `python3 tools/re/check_docs.py --staged` (the pre-commit hook runs it
   too; install it once with `tools/re/install-hooks.sh`). Fix every error.
   Never use `SKATE3_SKIP_DOC_CHECK=1` without the user's say-so.
4. Commit with the repo's configured git identity (`git config user.name`);
   push only when asked. After a wrap-up commit of a fix or feature,
   install the testing build on the device if it's connected and the user
   wants that (`tools/release/build-testing.sh --install`).
