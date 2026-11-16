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

Crossing a board-sales milestone happens on an award, not on a write. When
the harness sets the total just under 300,000 and a challenge pays out, the
game shows, in order:

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
four counters add up to N. Example and instructions:
`scenarios/career_board_sales.toml`. Reaching a milestone screen still takes
a challenge award: set the total just under it, then complete any challenge.

## Not known yet

- Where the milestone check runs (the notifier, or the challenge results
  path), and whether calling it directly shows the screens without a
  challenge.
- What `+0x08`..`+0x10` count, and how the save stores `S`.
- Challenge completion state (`CompleteChallenge`, `sub_8283FD00`).
