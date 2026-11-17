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
| Title (FE 22) | `ReceiveInput` `sub_82609238`: Start, after a 1.5 s delay and its animation | any button shows the prompt at once; A works like Start (`skate3_title_quick_start`) |

"Any button" is every digital button except Guide, on a rising edge. One
detector is shared by the logo and the movie, so a held button skips only
one screen. After the title, frontend movies skip on A or Start only, as
before. The skip reads the buttons the game itself received
(`XInputGetState` `sub_82EE22C0`, after any harness rewrite) and the host
UI pad.

With a press about a second into each screen, the screens before the title
took 5.2 s instead of 24.5 s (`scenarios/boot_skip.toml`; Windows and the
Odin 2 alike, a held X skipped only the logo).

**Harness automation** (`boot = "demo"`, the `skate3_demo_path` cvar) skips
without presses: the movie through `ShouldForceIntroMovieComplete` and, since
2026-11-17, the logo through `ShouldForceIntroLogoComplete` (the logo ended
after 0.57 s instead of 4.7 s). The language select and the title are passed
with the demo path's A taps. Scenarios that test the boot screens themselves
(`boot_timing`, `boot_skip`, `title_*`, `native_tour_boot`) use
`boot = "none"` and keep the natural timings.

## The title screen (FE state 22)
**Why the prompt comes late.** Two clocks, measured 2026-11-16:
- The state's `OnUpdate` `sub_82609050` adds `dt` to a global millisecond
  counter `0x8308355C` until it passes 1500, then sets the ready byte
  `this+72`. Only then does input confirm (it is reset to 0 on confirm).
- The picture is the title's APT (Flash) animation: background fade-in,
  the logo built letter by letter (0.9-1.5 s), the "Press Start" line
  (about 1.8 s). It runs on APT's own clock and ignores the ready byte.
  Setting `+72` early changes nothing on screen.

After 30 s idle with the prompt up, `OnUpdate` clears `+72` and plays the
intro movie again (FE state 24, attract mode).

**Input.** The FE delivers each pad button to `ReceiveInput`
`sub_82609238` as a code on press and code + 44 on release (measured with
the probe, `scenarios/title_input_probe.toml`):

| Button | Start | Back | A | B | Y | X | LB | D-pad down |
|---|---|---|---|---|---|---|---|---|
| Press / release | 0 / 44 | 1 / 45 | 2 / 46 | 3 / 47 | 4 / 48 | 5 / 49 | 9 / 53 | 13+17 / 29+33 |

Every code resets the idle timer (`+76`, `+64`). Press 0 arms `+80`;
release 44 confirms when `+72` (ready), `+80` (armed) and not `+60`
(busy) hold: it resets the delay counter, plays the confirm sound
(`sub_825DFAF0`), records the pad (`fe+260`) and calls `sub_82609188`.
The boot handler `ShowPressStartMode` then sees `+60` and moves on (the
autosave warning, then the profile flow). Nothing reacts to A.

**APT's clock.** `sub_82E663D0(elapsed_ms)` steps the current movie
(`*(*(0x83085768)+24)`; one root movie holds every frontend screen):
it adds `elapsed` to the movie's accumulator (`+36`), advances one frame
per frame duration (`+44`, ms) and adds that to APT's clock
`0x8307F3C8` (ActionScript `getTimer()`, `sub_82E6DFD8`).

**Quick title** (`skate3_title_quick_start`, default on; "Quick Title
Screen" in the options menu; `src/skate3_demo_path.cpp`):
- A fresh press of any button during the delay ends it (counter to 1501)
  and adds 3,000 ms to APT's next step, so the logo and the prompt are up
  at once (0.3 s after the title appears instead of about 1.8 s).
- That press doesn't confirm: it clears `+80`, and Start / A presses don't
  arm until every button is up again. A second press is needed.
- A works like Start: its codes 2 / 46 are passed on as 0 / 44.

## Before the boot flow: the game tick and its pad-sample rings
The boot flow (handler 1 on) runs inside the game tick, and the game tick
runs only when a pad sample is waiting. Found 2026-11-16 chasing a slow boot
at a frame cap (below).

**`Sk8::Sk8AppMan`** (vtable `0x8230C880`; Skate 2 names `Init` at +0x38):
- slot +0x44 `sub_826D34B8`, the per-tick update (main thread, called from
  the main loop `sub_82965C90`): pops a sample from ring 0
  (`sub_82699230(appman+128, 0)` -> `sub_8296D1F0`); if there is none it
  returns 0 and nothing else runs. With a sample it updates the game systems
  and, while `appman+125` is set, the flow machine (`sub_826DF3B8(appman+156)`,
  which makes the boot flow's first transition on its first tick). So game
  logic runs once per pad sample, not once per frame.
- slot +0x48 `sub_826D6098`, the pad sampler (its own thread, about 60 Hz;
  probably Skate 2's `TimerCallback`): polls the pads (`sub_82698FA8`) and
  pushes one sample into each ring (`sub_8296D0D0`).
- **The rings** (`appman+128` -> `[6]`, `[7]`): 30 slots of 4 pad records
  (100 bytes, at `q + 4 + (slot*4 + pad)*100`); read index `q+12004`, write
  index `q+12008`, pad count `q+12012`, source buffer `q+12016` (its current
  half at `+804`; pad i's record is `2*i + half`). Pop: `next = (read+1) % 30`,
  empty when `next == write`. `sub_826991B8(holder, idx)` withdraws the last
  push (`write = (write+29) % 30`). Native: `src/native/skate3_native_padring.cpp`.

**The main loop and the render loop share a mutex.** The main loop
(`sub_82965C90`) locks `EA::Thread::Mutex` `this+24` (`sub_82A40368`) around
each tick. The render loop (`sub_829661F0`, render thread) reads its frame
stopwatch (`Stopwatch::GetElapsedTimeFloat` `sub_82AEDA18`), locks the same
mutex, renders and D3D-swaps through the presenter (vtable +32), unlocks and
loops: it holds the mutex for nearly the whole frame and releases it for a
few instructions.

**The slow boot at a frame cap (fixed 2026-11-16, SDK).** On the Odin 2 the
language select took 0.1-77 s at the 30 fps cap (0.5-4 s uncapped; Windows
always ~1.2 s; upstream the same). A wall-clock profile (`profile` step
`clock = "wall"`, `profile_report.py --waits`) put the main thread 83% in
`EA::Thread::Mutex::Lock`: the render thread held the mutex through the cap
wait (inside Swap) and took it straight back after each unlock. The mutex is
an Xbox critical section whose waiters sleep on an auto-reset event; the
SDK's POSIX auto-reset event was a flag that the first thread to look
consumed, so the releasing render thread, re-entering at once, beat the
woken main thread nearly every time. The Xbox 360 and Windows kernels hand a
set event to a thread already waiting; the SDK now does the same on POSIX
(FIFO hand-off; `REX_EVENT_HANDOFF=0` restores the old behaviour for A/B).
Moving the cap wait outside the mutex also fixed the boot but cost gameplay
1% lows (Windows 29 -> 25 fps), so it was dropped.

## The FE state objects (TU3)
From the state-table dump (`skate3_demo_path_probe`, `FE state N obj=...
vt=...` lines). Vtable slots: +0x0C enter, +0x10 `OnUpdate(this, dt_ms)`,
+0x18 leave, +0x28 `ReceiveInput(this, ?, input)`.

| FE state | Vtable | Enter | Update | Leave | Input |
|---|---|---|---|---|---|
| 47 language | `0x82304E70` | `826393E8` (opens screen 94) | `82639400` | `826504C8` | `82639440` |
| 5 logo | `0x82302D90` | `82608E00` (opens screen 5) | `82608E38` | `82608E18` | - |
| 24 movie | `0x82303390` | `8260AFD8` | `8260B058` | `8260B150` | `8260B240` |
| 22 title | `0x82302FB0` | `82608F18` | `82609050` | `82608F58` | `82609238` (slot +0x2C `82608EA0` sets the mode `+68`) |

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
