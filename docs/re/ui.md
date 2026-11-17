# The frontend and HUD (RE-9)

The game's menus and HUD: what drives them, what we already hook, and the plan
to replace them with native code. Not a priority yet (2026-11-17). The goal is
that new game-style UI (settings pages, new menus, on-screen hints such as the
free-fly controls) becomes ordinary native code instead of one-off hooks.

## What it is

- **Screens are Flash movies run by EA's APT player.** Each FE screen id maps
  to a `.swf` (`fe_screens.tsv`, `tools/re/fe_screens.py`; table at
  `0x8302F490`). The shipped files are `.apt` + `.const` (+ `.geo`) in
  `data/big/fedata.big` (`tools/re/big.py extract`, never commit them). APT
  advances each movie on its own clock (`sub_82E663D0`, see the title screen
  in [boot.md](boot.md)), not on the game's FE logic.
- **FE states drive the movies.** A `FrontEndState_*` object (`OnEnter`,
  `OnUpdate`, `ReceiveInput`, `OnLeave` in its vtable) is pushed on the
  FrontEndManager's stack (`0x830CFE14`; push `sub_82D0ABE0` / `AB30` /
  `AEF0`). State ids are not screen ids: `*(*(mgr+8)+4*id)` is the state
  object ([boot.md](boot.md), "The FE state objects").
- **Movies pull their data through natives.** ActionScript calls game
  functions registered as APT natives (`GameSettings_*` for the Options
  screen, `GetTutorialHalText` / `GetTutorialGesturesButtons` for the
  tutorial panel). Lua scripts reach the HUD through bindings
  ([names_bindings.tsv](names_bindings.tsv), table `0x823147B4`:
  `ShowCustomCentralMessage`, `ShowSkateMessage`, `TutorialInstructionSet`,
  `SetActiveGroup`, ...).
- **Text** goes through `GetLocaleText` (`sub_825AA818`): string IDs such as
  `ID_GAMESETTINGS_VSYNC_TOGGLE` are hashed and looked up in the localizer.

## Input: when a screen accepts it (2026-11-17)

`sub_825EC0E8` (the state manager's input dispatch; `S = *(0x830CFE14) +
0x200`, the object whose stack the harness reads at `+0x210`) hands an FE
input code to the top screen's `ReceiveInput` (vtable `+0x28`) only if:
- the queued-state list `S+32..+36` (20-byte records) is empty,
- the lock byte `S+12` is clear,
- `*(*(0x830354EC + 28)) + 11264` is zero,
- `sub_825EB998` doesn't claim the code,
- and the top state object (`*(S+8)[id]`) is **active**: `+16 == 3`
  (measured: 1 while entering, 2 while its intro plays, 3 active), byte
  `+49` set, word `+52` zero.

Popups (`S+112..+116`) take the input first (state 29). Measured on Windows
(`census_menus`): only the state went through 1 -> 2 -> 3 on each screen
change; the lock, queue and global stayed 0. The time to "active": language
select ~0.2 s (why an early A was dropped), pause menu ~0.1 s,
Create-a-Skater ~0.45 s. The harness exposes this as `fe_ready` (`fe_input`
events, `wait fe_ready`, and `input` waits for it while a menu is up).

## FE state methods (2026-11-17)

Every FE state's OnEnter is vtable `+0x0C` and OnLeave `+0x18`; OpenScreen
is `+56` (this, screen, 0) and CloseScreen `+60` (this, 0). Many enter /
leave methods are a few instructions; 23 of them run as native code
(`skate3_native_boot.cpp`, `skate3_native_fe.cpp`, catalogued in
[replacements.md](replacements.md)). The harness `fe` step pushes or pops
any state through the game's own calls, which is how they were exercised.

## What we already hook

`src/skate3_options_menu.cpp` (the recomp settings inside the game's Options
screen, `options.apt`):

- **The Options tables:** `kOptionTable` `0x83026EC8` (36 x {title string ID,
  widget type "option" / "slider" / "selector"}) and `kScreenLayouts`
  `0x83026FE8` (int32[13][16] option ids per screen). Unused option ids are
  borrowed for our root rows; our pages are screen ids past the game's 13.
  `FrontEndState_Options` vtable `0x823034B0` (+64 OnMenuSelect
  `sub_8260D290`, +68 highlight `sub_8260D710`, +72 item-enabled
  `sub_8260D648`, +40 input: Back 3, Left 14, Right 15); the state keeps the
  screen at +68 and the row at +72.
- **Our own text:** a hook on the localizer's lookup-by-hash answers our
  host-owned string IDs.
- **Popups:** a descriptor built by `sub_82D0B0B0` (title, message, buttons,
  guest callback), shown by `sub_82D0A8D0` on the popup manager
  (`0x830CFE1C`; queue at +112/+116, popup id at +1428; answered through
  `sub_825EF698`, flushed by `sub_82D0A738`, closed by id `sub_82D0A628`).
- **FE sounds** by hash (submenu, back, selector left/right).
- Also: the title screen's input and update (`sub_82609238`, `sub_82609050`)
  and the language select (`sub_82639400`), in `src/skate3_demo_path.cpp`.

These are hooks for one screen each, not a reusable layer.

## Movies worth knowing (fedata)

| Movie | What it is | Interface seen in its strings |
|---|---|---|
| `hudtutorial` (FE screen 3, TutorialInstructionHUD) | The "Coach Frank says" panel: a text line and up to three button items with stick-gesture icons | `GetTutorialHalText`, `GetTutorialGesturesButtons`, `SetButtonType`, `SetGestureType`, `mcButtonItem1..3`; Lua `TutorialInstructionSet` |
| `button_menu`, `button_item2` | The button-help bar along the bottom of menus ("A Select, B Back") | `AddButtonItem`, `ClearButtonItems`, `RefreshButtonHelp`, `buttonHelpVisible`, `mButtonImage` |
| `core_menu` (FE screen 22, CrossBar) | The main frontend menu | |
| `options` | The Options screen (above) | `GameSettings_*` natives |
| `small_popup`, `sk8popup`, `fmv_popup` | Popups | |
| `hudcentral`, `centralhud` | Central HUD messages | Lua `Show*CentralMessage`, `ShowSkateMessage` |
| `panel`, `menu_picker`, `dynamic_selectable_list`, `highlight_*`, `basic_slider`, `scrollbar`, `tabs` | Menu building blocks | |
| `tiploader`, `tutorial`, `skate101`, `trickguide` | Tips and tutorials | |

## Plan (RE-9)

1. **Tooling:** `tools/re/apt.py` lists a movie's sprites, frame labels,
   ActionScript functions and the natives it calls (from the `.apt` /
   `.const` files), and maps natives to their registration in the binary.
2. **Map the native interface** for the movies above: who registers each
   native, its arguments, the data it returns (`symbols.toml`, rows in
   [replacements.md](replacements.md)).
3. **Map the FE state lifecycle** for one menu end to end (Options first, as
   most of it is hooked): enter, input codes, refresh modes, leave.
4. **Replace in native code,** one piece at a time, the usual way (Body
   families, catalogued in replacements.md): the Options state's handlers,
   the popup builder, the tutorial panel's data natives, the button-help bar.
5. **Then the UI features** sit on that layer: a reusable game-UI library
   (rows, pages, popups, text, sounds) out of `skate3_options_menu.cpp`; the
   free-fly hint in the tutorial panel; the button-help bar on our pages;
   new standalone menus on a reused menu movie.
