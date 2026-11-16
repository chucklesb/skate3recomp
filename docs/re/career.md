# Career and progression

How Skate 3 keeps career progress (board sales, cash, milestones), as far as
known, and how the harness sets it. Functions: [symbols.toml](symbols.toml)
("Career and progression"). Roadmap RE-4.

## Board sales

- **Not one number.** The career stats object `S` holds four u32 counters at
  `S+0x04`, `+0x08`, `+0x0C` and `+0x10`. The total the game shows is their
  sum: the front end's `GetTotalBoardSales` (`sub_825B1630`) adds them.
  Searching memory for the total finds nothing.
- **Where `S` is:** `S = *(*(*0x830CFD94 + 0xE0) + 0x48)`. The career
  object `K = *(*0x830CFD94 + 0xE0)` also holds the challenge tracker at
  `K+0x44` and the last challenge's rewards at `K+0x60`. It was at
  `0x4020D180` in every session so far (Odin 2, 2026-11-15).
- **The counters** come from the challenge payout, `sub_827CDDD8(career,
  challenge, level, online)`. Completing a challenge pays its Owned reward
  (cash and board sales from the challenge's data); a Kill (level 2) adds
  the Killed reward on top:
  - **`+0x04`: Owned board sales.** The Lua binding `AwardBoardSales`
    (`sub_827D1328`) adds here too, and a challenge award landed here
    (269,000 → 281,500, Odin 2, 2026-11-15).
  - **`+0x08`: Killed board sales** (30,000 on the Odin 2 test career, 73,750
    on Windows).
  - **`+0x0C`: online board sales** (the payout's online part; also
    `sub_827CEC78`, called from two online handlers).
  - **`+0x10`: board sales awarded by the online service**, set (not added)
    from the `awardedBoardSales` field of the skate.Online login response
    (`sub_827CECE0`, called by the response handlers `sub_825F23B0` and
    `sub_826EFD70`). The response's fields are `privacyFlagContainer`,
    `awardedBoardSales` (64-bit) and `teamInfo`, read at `+0x08`, `+0x10`
    and `+0x18`; the login request (`StartLoginProcess`) sends
    `totalBoardSales` and `difficulty`. Likely the royalties for other
    players downloading or voting on your content (`UpdateNewRoyaltyDownloads`,
    `UpdateNewRoyaltyVotes`). EA's servers are gone, so it stays 0 (it is 0
    on both test careers).
  - The reset to 0 for a new career is in `sub_825F2C98`.
- **The last challenge's rewards:** `R = *(career+0x60)` (also the results
  presenter's `this+0x5C`). The payout adds to it: `+0x3C` base cash,
  `+0x40` killed bonus cash, `+0x44` online, `+0x48` owned and `+0x4C`
  killed board sales. The front end reads them back (`GetBaseCashReward`,
  `GetKilledBonusCashReward`, `GetOnline/Owned/KilledBoardSalesReward`,
  `sub_825B15A0`..`sub_825B1600`).
- **Cash:** `S+0x14`, which `GetTotalCash` returns. `AwardCash` adds to
  `+0x14` and `+0x18` (a lifetime total, probably).
- **`S+0x1C`: the actions tracker** `A` (context of the Lua `Actions` bindings,
  `ActivateAction`/`DeactivateAction`). `A+0x04` is the list of locked
  unlock actions (count at `+0x0C`), which the unlock check
  (`sub_82C1C2A0(A)`, below) fires; `A+0x24` points at the challenge tracker
  and `A+0x28` back at `S`.

## Challenges

The challenge tracker `C = *(A+0x24)` is the context of the Lua challenge
bindings (`IsChallengePassed`, `GetChallengeCompletionLevel`,
`GetNumCompletedChallenges`, ...). It holds three lists, 16 bytes apart, each
a circular list head with its count 8 bytes later; a node carries the
challenge's 64-bit name hash at `+8`:

| C+ | Count at | Holds | Windows test career |
|---|---|---|---|
| `0x04` | `0x0C` | active challenges | 58 |
| `0x14` | `0x1C` | **Owned** | 27 |
| `0x24` | `0x2C` | **Killed** | 28 |

`sub_82C1D988(C, hash)` returns a challenge's completion level: 0 Owned, 1
Killed, 2 not completed (the script enum `EChallengeCompletionLevel` is
{Owned, Killed}; `eHighScoreTier` adds PersonalBest).
`GetNumCompletedChallenges` is Owned + Killed.

**Names:** challenges (and unlocks) are keyed by the game's 64-bit name
hash, `sub_82B73AC0`: Bob Jenkins' lookup8 with `a = b = 0xABCDEF0011223344`
(confirmed: `hom_univ_04_sp` hashes to the Owned list's `91941878C48D7834`).
Names look like `<type>_<district>_<NN>[_suffix]`: `film_dwtn_01`,
`photo_univ_03`, `race_indu_02`, `hom_univ_04_sp`, `tut_sksc_01`.
Every challenge, unlock action and other data record is named in
[attrib_names.tsv](attrib_names.tsv) (below). `tools/re/challenges.py hash`
hashes any name; `challenges.py show SNAP` lists a career snapshot's
challenges and locked actions by name.

**From the harness:** `career` with `complete = "name"` (or `"0x<hash>"`)
and `level = "owned"` or `"killed"`:

- **With `pay = true`** it does what a challenge's end does: it finds the
  challenge's record object (vtable `0x82326AF4`, name hash at `+8`; one per
  challenge, in two arrays on the heap) and calls the career's
  `sub_827CDB10(K+4, record, level)`. That pays the Owned reward (and the
  Killed reward for `killed`) only if not already earned, records the
  level, runs the unlock check, and raises the results presenter's flag
  itself, so the results follow (Rewards with the amounts, unlock cards,
  sometimes New Teammate and a cutscene). Windows, 2026-11-15:
  `film_indu_02` Killed paid 10,000 Owned + 2,500 Killed; `street_dwtn_01`
  paid 20,000 and unlocked Street Ender.
- **Without `pay`** it calls only the tracker's `sub_82C1D878`: the
  challenge leaves the active list and goes to Owned or Killed, and unlock
  actions gated on it react (`photo_univ_03`: active 64 → 63, Killed
  28 → 29, locked actions 53 → 52). Nothing is shown or saved.

**Saving:** the game saves the career by itself after the results are
walked to the end, including the cutscene some end with (FE 24), with the
save indicator bottom right. Quitting before the save finishes loses the
change. The harness `results` step walks the results ("Continue" on New
Teammate: its default, Customize Teammate, opens Create-a-Skater) and
returns only once `SKATER.P` has been rewritten and left alone for 3 s.
Checked by a second run: the paid counters and the Killed level were kept,
and nothing was paid twice. On the Odin 2 too (run
`20261115-213444_career_complete_challenge`: `street_dwtn_01` paid 20,000,
281,500 → 301,500, saved to the device's `SKATER.P`). Earlier harness runs
that quit at the results were not kept (the device career was back at
281,500 after the 1,000,000 milestone run). It runs on the Odin 2 too (run `20261115-210836_career_complete_challenge`; that career tracks 89 active, 30 Owned, 15 Killed). It pays no reward (set `board_sales` for that).
`scenarios/career_complete_challenge.toml`.

**Completing a challenge, as the game does it:** the Lua `CompleteChallenge` (`sub_8283FD00`)
ends up in a career method, `sub_827CDB10` (`this` = `K+4`; vtable slot at
`0x823117EC`), with (challenge, level). It runs the payout
(`sub_827CDDD8`), then records the level with `sub_82C1D878(C, hash,
killed, online)`, which moves the challenge onto the Owned or Killed list.
The challenge object it takes (vtable `0x82326AF4`) has the name hash at
`+8`; the harness skips it and calls `sub_82C1D878` directly. The `career` step reports
these counts (`challenges`, `locked_actions`).

## The game's data: names for every key

The `.big` archives (`data/big/*.big`, EA "EB" version 3, RefPack
compression) are read by `tools/re/big.py` (`list`, `extract` to
`out/re/big/`, `grep`). The databases inside are EA Attrib vaults
(`.vlt`/`.bin` pairs), each with a summary report naming its records as
`/<class>.class/<record>.xml`. The code looks records up by the lookup8 hash
of the class and record names, so `tools/re/attrib.py names` turns the
reports into [attrib_names.tsv](attrib_names.tsv): 8,870 names in 329
classes, and `attrib.py lookup` resolves a constant. For the career:

- `0xA5970491380FF1BE`, the table the unlock check reads, is the class
  **`progression_action`**; the 53 locked actions on the Windows test career
  are all its records, e.g. `branding_level_10_complete`,
  `stadium_contest_complete`, `team_film_unlock_complete`,
  `skateschool_unlock_intermediate`, `thrasher_1_unlock_complete`.
- `0x4429D52046FC5D6D` is `progression_action/milestone_actions`.
- `GetNextBoardSaleMilestone`'s gate (`sub_827CD728`) tests
  `challenge_global_data/photo_univ_05`.
- Challenges are `challenge_global_data` / `challenge_local_data` records
  (the table keeps one class per name); every one the test careers
  track has a name. Other progression classes: `progression_state`,
  `progression_handler`, `progression_rewards`, `progression_stategraph`,
  `progression_group`.
- Not covered: attribute (field) keys such as the payout's reward fields
  (`0x8915E3FCB76529BA`, `0x3DBC2F9C0083DF86`, `0x2E4824C81FDAE87C`); the
  reports list records, not fields.

## The save

The career save (`ALIAS_SKATER/SKATER.P`, 2.6 MB, an `MC02` container) stores
`S` field by field, each as a big-endian (size = 4, value) pair after a
record header with hash `CFA69F92`: the four counters, then cash (`+0x14`)
and `+0x18`. Seen in the Windows save on 2026-11-15 (offset `0x11464C`;
4,926,250 / 73,750 / 0 / 0 / 2,500 / 2,500). The container likely has a
checksum, so the harness changes the live object and lets the game save.
- **Skate 2 has none of this:** its progression is coverage, medals, sponsors
  and unlocks (`Sk8::Progression::tCompletionResults`; `sk2_types.py`).

## Milestones

Crossing a milestone takes two things the game does after an award, not the
write itself:

1. **The unlock check.** After adding to the counters, the award calls
   `sub_82C1C2A0(A)`, `A` being the actions tracker `*(S+0x1C)`. It keeps a
   list of locked unlock actions at `+0x04` (circular, a 64-bit hash at
   node+8; 61 on the Windows test career). For each, the check tests its
   requirements (`sub_82C1CA48`) and fires the ones now met: they leave the
   list (10 fired going from 537,500 to 1,500,000), and Lua runs on the
   tracker's context. Nothing shows yet.
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

The Odin 2 showed the same sequence from 311,500 to 1,000,000 (run
`20261115-204935_career_milestone`).

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
- How the save's record is found and checked (header hash, checksum).
