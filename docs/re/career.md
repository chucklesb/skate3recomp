# Career and progression

How Skate 3 keeps career progress (board sales, cash, milestones), as far as
known, and how the harness sets it. Functions: [symbols.toml](symbols.toml)
("Career and progression"). Roadmap RE-4.

## Board sales

- **Not one number.** The career stats object `S` holds four u32 counters at
  `S+0x04`, `+0x08`, `+0x0C` and `+0x10`. The total the game shows is their
  sum: the front end's `GetTotalBoardSales` (`sub_825B1630`) adds them.
  Searching memory for the total finds nothing.
- **Where `S` is:** `S = *(*(*0x830CFD94 + 0xE0) + 0x48)`. It was at
  `0x4020D180` in every session so far (Odin 2, 2026-11-15).
- **The counters:**
  - **`+0x04`** is where awards go. The Lua binding `AwardBoardSales`
    (`sub_827D1328`) adds its argument there, and a challenge award landed
    there too (269,000 → 281,500, 2026-11-15).
  - **`+0x08`** held 30,000 on the test career. Not identified; the front end
    also has `GetOnlineBoardSalesReward`, `GetOwnedBoardSalesReward` and
    `GetKilledBoardSalesReward`, likely sources for the other counters.
  - **`+0x0C`, `+0x10`:** 0 on the test career.
- **Cash:** `S+0x14`, which `GetTotalCash` returns. `AwardCash` adds to
  `+0x14` and `+0x18` (a lifetime total, probably).
- **`S+0x1C`:** a pointer. When it is set, `AwardBoardSales` calls a notifier
  (`sub_82C1C2A0`, not examined yet).
- **Skate 2 has none of this:** its progression is coverage, medals, sponsors
  and unlocks (`Sk8::Progression::tCompletionResults`; `sk2_types.py`).

## Milestones

Crossing a milestone takes two things the game does after an award, not the
write itself:

1. **The unlock check.** After adding to the counters, the award calls
   `sub_82C1C2A0(watcher)`, the watcher being `*(S+0x1C)`. The watcher keeps
   a list of locked unlocks at `+0x04` (circular, a 64-bit hash at node+8;
   61 on the Windows test career). For each, the check tests its
   requirements (`sub_82C1CA48`) and fires the ones now met: they leave the
   list (10 fired going from 537,500 to 1,500,000), and Lua runs on the
   watcher's context. Nothing shows yet.
2. **The results presenter.** An object whose update (`sub_827CF570`,
   virtual, vtable slot at `0x82311800`) runs every frame. When its flag at
   `this+0x30` is set and the game isn't busy, it pushes FE 64; a finished
   challenge sets the flag. Its reward object is at `this+0x5C`
   (`sub_827C58B8` tests its pending fields).

The harness does both: `career` with `notify = true` and `show = true`
(`scenarios/career_milestone.toml`). Each alone shows nothing. On Windows,
from 537,500 to 5,000,000 (2026-11-15), the game showed:

| FE stack | Screen |
|---|---|
| `[0,64]` | Rewards: the challenges unlocked (Team Coverage, Team Films), Board Sales Milestone 5/6 |
| `[0,65]` | Milestone Unlocked! cards, one per A press: 6 (1,000,000 boards sold), Monster Contest, 5 (600,000), Goofy vs. Regular |
| `[0,66]` | New Teammate! (pushed by `sub_826504E0`) |
| `[0,46]` | Receipt, after it |

`sub_827CD150` pushes 65 and 66. The FE ids are indexes into the front
end's screen table (`0x8302F490`; all 86 names in
[fe_screens.tsv](fe_screens.tsv), `tools/re/fe_screens.py`): 64
MilestoneUnlock, 65 TeammateUnlock, 66 FirstTeamMember, 22 CrossBar (the
pause menu), 46 Receipt.

From the Odin 2, with a challenge award crossing 300,000, the game showed:

| FE stack | Screen | Emulated draws/frame (Odin 2) |
|---|---|---|
| `[0,64]` | Milestone Unlocked! (Fourth Board Sale Milestone, 300,000 boards) | 10.0 |
| `[0,65]` | Milestone Unlocked! (the location it unlocks: Downtown Plaza) | 10.0 |
| `[0,66]` | **New Teammate!** Customize Teammate / Preset Skaters / Continue, with the teammate's portrait | 36.4 (pitch 1200: 26.0, the skater-photo window) |
| `[0,66,15]` | Customize Teammate (Create-a-Skater) | 12.2 |

Run `20261115-201900_live`. Strings: `unlock/unlock_milestone.swf`
(`MilestoneUnlock`), `unlock/unlock_teammate.swf` (`FirstTeamMember`,
`TeammateUnlock`); messages `cMsgPresTeamBrandTeammate`,
`cMsgPresTeamBrandUnlock` ([messages.tsv](messages.tsv)).

## Setting it from the harness

`do = "career"`, `board_sales = N` sets the total: it rewrites `S+0x04` so the
four counters add up to N (`scenarios/career_board_sales.toml`). Add
`notify = true` and `show = true` to cross milestones and get their screens
without a challenge (`scenarios/career_milestone.toml`, above). Live:
`send career board_sales=N notify=true show=true`.

## Not known yet

- What sets the presenter's flag at the end of a challenge, and the
  presenter's class (no RTTI; Skate 2 has no counterpart).
- The FE stack's third entry: `[0,66,15]` was Customize Teammate, but table
  id 15 is MessengerScreen, so later entries may not be screen ids.
- What `+0x08`..`+0x10` count, and how the save stores `S`.
- Challenge completion state (`CompleteChallenge`, `sub_8283FD00`).
