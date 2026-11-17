# Boot flow (power-on to the title screen)

What Skate 3 (TU3) runs between launch and "Press Start", what each
screen waits for, and what a native replacement has to keep. Roadmap
RE-8; native code in `src/native/skate3_native_boot.cpp`, the skip in
`src/skate3_demo_path.cpp`. Measured on Windows, 2026-11-16
(`scenarios/boot_timing.toml`, `tools/harness/boot_timing.py`).

## The sequence
`Sk8::Flow::StateBootFlow` (Skate 2 name) drives the boot as a chain of
handler functions `handler(this, event)`. The events are 0 (enter),
1 (update, once a frame) and 4 (leave). A handler moves on with
`sub_826DFB30(this, next_handler, 1, 0)`. The handlers push frontend (FE)
states. These are FE **state** ids, the index into the FrontEndManager's
state table (`*(mgr+8)`, one object per id), not the screen ids of
[fe_screens.tsv](fe_screens.tsv), even though 47 and 22 happen to match.

| # | Handler | What it does | Ends when | Lasted (30 / 60 / ~500 fps) |
|---|---|---|---|---|
| 1 | `sub_826FDC50` `LoadBootFlow` | creates the boot game module (176 bytes, vtable `0x8230C748`, `sub_826D6A68`) unless one exists; pushes FE state 0 | at once | - |
| 2 | `sub_826FDD70` language | pushes FE state 47 if `this+56` is set; on leave stores the language | FE 47 closes | 10.60 / 10.60 / 10.59 s |
| 3 | `sub_826FDE48` logo | pushes FE state 5 (the Skate 3 logo and "©2010 Electronic Arts") | the logo state's done byte (`+64`) | 4.70 / 4.70 / 4.70 s |
| 4 | `sub_826FDEF0` movie | sets FE state 24 (`SetFrontEndState`) and starts the FE movie player: one FMV, the EA logo then Black Box. Once the player reports "playing", it sets the boot module's load phase to 1 | the player's state reaches 4 (done) | 9.20 / 9.07 / 8.98 s |
| 5 | `sub_826FE058` wait for load | shows the loading UI (`sub_825DF470(fe, 1, 1)`) | the module's load phase is 3, or its byte `+32` is set | 0.02 s (the load was done) |
| 6 | `sub_826FE138` title | hides the loading UI, pushes FE state 22 (`sub_82D0AEF0`) | at once | - |
| 7 | `sub_826FE1D8` `ShowPressStartMode` | waits on the title for Start, then the profile and save flow | Start | - |

## Frame-rate dependence: none
Every timed screen accumulates the frame's `dt` in milliseconds (the
FrontEndState's `OnUpdate(this, dt_ms)`, vtable +0x10), not frames:
- **Language select**: `OnUpdate` `sub_82639400` adds `dt` to `this+60`.
  At 10,000 ms it pops itself, keeping the current language. That is the
  10.6 s seen with no input (plus the screen's fade).
- **Logo**: `OnUpdate` `sub_82608E38` adds `dt` to `this+60`. At 4,000 ms
  it sets the done byte `this+64`, which handler 3 polls.
- **Movie**: the FMV plays in real time; the movie player's state word is
  `*(*(fe+264)+12)` (1 starting, 2 playing, 4 done; `fe = *(0x830CFE14)`).

At 30, 60 and ~500 fps (uncapped) each screen lasted the same time to
within 0.2 s while the frame counts scaled with the rate (logo: 141, 282
and ~2,430 frames).

## Blocking: the screens don't wait for loading; loading waits for them
- **The main loop is never blocked.** It presents every frame during the
  boot screens.
- **The boot game module's load phase** (`module+68`, an atomic word;
  the module is `*(*(0x83083BCC)+8)`, checked as type 6 by
  `QueryModule` `sub_82471EA8`) stays 0 through the language select and
  the logo, about 15 s. Handler 4 sets it to 1 when the movie starts
  playing. The probe saw 0 → 2 → 18 and the done byte `+32` set about
  0.35 s later (Windows).
- **So the language select and the logo are pure waits in front of
  the load.** Nothing loads behind them, and skipping them moves the load
  earlier. Skipping the movie is safe: handler 5 then shows the loading
  UI until the load is done (on Windows it already is).
- **The Odin 2** (2026-11-16, 30 fps) is the same: language 10.63 s, logo
  4.70 s, FMV 9.20 s; the load is done 0.48 s after the movie starts.
  The time from launch to the first screen varies there (1.3 s in one
  run, 10.9 s in two others), so scenarios wait for each screen
  (`wait fe_screen = N`), not a fixed time.

## Input today
| Screen | The game's own input | Our skip (`skate3_intro_movie_skip`, default on) |
|---|---|---|
| Language (FE 47) | `ReceiveInput` `sub_82639440`: 12/13 move through the 6 languages, 2 picks one and pops | none needed |
| Logo (FE 5) | none (no `ReceiveInput`) | any button: the native `OnUpdate` sets the done byte |
| Movie (FE 24) | `ReceiveInput` `sub_8260B240` pops it, but only when `this+65` ("skippable") is set, which the boot movie doesn't set | any button: completes the FMV (`FEMoviePlayer::Update` codegen patch, `ShouldForceIntroMovieComplete`) |
| Title (FE 22) | `ReceiveInput` `sub_82609238`: input 44 (Start) | none needed |

"Any button" is every digital button except Guide, on a rising edge. One
detector is shared by the logo and the movie, so a held button skips only
one screen. After the title, frontend movies skip on A or Start only, as
before. The skip reads the buttons the game itself received
(`XInputGetState` `sub_82EE22C0`, after any harness rewrite) and the host
UI pad.

With a press about a second into each screen, the screens before the title
took 5.2 s instead of 24.5 s (`scenarios/boot_skip.toml`; Windows and the
Odin 2 alike, a held X skipped only the logo).

## The FE state objects (TU3)
From the state-table dump (`skate3_demo_path_probe`, `FE state N obj=...
vt=...` lines). Vtable slots: +0x0C enter, +0x10 `OnUpdate(this, dt_ms)`,
+0x18 leave, +0x28 `ReceiveInput(this, ?, input)`.

| FE state | Vtable | Enter | Update | Leave | Input |
|---|---|---|---|---|---|
| 47 language | `0x82304E70` | `826393E8` (opens screen 94) | `82639400` | `826504C8` | `82639440` |
| 5 logo | `0x82302D90` | `82608E00` (opens screen 5) | `82608E38` | `82608E18` | - |
| 24 movie | `0x82303390` | `8260AFD8` | `8260B058` | `8260B150` | `8260B240` |
| 22 title | `0x82302FB0` | `82608F18` | `82609050` | `82608F58` | `82609238` |

## What a native boot needs (requirements)
To replace the boot screens with native code, the replacement must keep:
1. **The boot module.** Create it as `LoadBootFlow` does, and set its load
   phase to 1 to start the load. Starting it at once instead of after the
   logo is allowed and saves the waits.
2. **The language.** Handler 2 stores the selected language
   (`sub_82D089F8` into `*(0x83085418)+828`) and calls `sub_825009F0`.
   Without the screen, the default or saved language must still be set
   the same way.
3. **The wait for the load.** Nothing may run the title or the profile
   flow before phase 3 (or `+32`).
4. **The FE stack the rest of the game expects:** state 0 at the bottom,
   then the title (22) pushed by handler 6 with its arguments, and
   `ShowPressStartMode` as the active boot handler. The profile, save and
   DLC flows after Start come from handler 7 and are out of scope here.
5. **The movie player's side effects.** Handler 4's enter writes a 64-bit
   key to `0x83026E68`, sets a byte at `*(*(0x830CFE0C)+28)+806`, adjusts
   the player at `fe+304` and calls `sub_824FE190(2)` (audio). These are
   not yet understood. Skipping the movie through its own completion (as
   today) keeps them.

What it can drop: the 10 s language timeout, the 4 s logo timer and the
FMV, as long as 1-4 hold. The next step (not done) is a native body for
the handler chain itself. The logo `OnUpdate` is the first native piece
(Body family `boot_logo`, `skate3_native_boot_replace`).

## Tools
- `scenarios/boot_timing.toml` + `tools/harness/boot_timing.py RUN...`:
  per-screen frames and seconds; run at several `--skate3_guest_fps_cap`.
- `scenarios/boot_skip.toml`: a press on each screen (a held one on the
  logo).
- `skate3_demo_path_probe`: logs the boot handlers' events, the load phase
  and movie state changes (`Skate 3 boot:` lines), FE pushes, and the FE
  state table once.
- `tools/re/vtable.py ADDR`: a vtable's slots with names.
