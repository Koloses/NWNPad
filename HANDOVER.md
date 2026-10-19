# NWNPad - handover

Gamepad mod for Neverwinter Nights: Enhanced Edition, Win64 build 8193.37-14 (git d950219).
State: feature complete for single player; all screens exercised under Wine with a virtual pad.

## How it hooks in

`version.dll` proxy (`src/proxy.cpp`, `src/version.def` forward the 17 exports to the system DLL).
`DllMain` only initialises inside `nwmain.exe`. Start-up (`src/main.cpp`):
log -> ini -> `G::Init` (game pointers) -> pad -> controls -> UI -> `Hooks_Install`
(checks the first 8 bytes of every used function against `src/sigs.inc`; any mismatch disables
the mod) -> UI hooks.

Hooks (MinHook):

| hook | why |
|---|---|
| `CClientExoAppInternal::ProcessInput` | reads the pad once per frame and runs gameplay / menu input before the game |
| `UpdateDriveMode` | analog movement through the game's own keyboard drive mode |
| `FeedbackHitCheckHostile/NonHostile`, `CGuiMan::GuiObjectHitCheck` | `g_hitOverride`: the game's own click / radial / targeting code acts on the pad soft target |
| `UpdateMouseCursorAppearance` | hover highlight + name plate follow the soft target |
| `gdi32!SwapBuffers` | draws the overlay (GL 3.3, SDF shapes and text) |
| `ShowDialogEntry / ShowDialogReplies` | feeds the conversation window |
| `AppendToMsgBuffer` | HUD message feed |
| `CGuiMan::ShowTooltipText` | captures quickbar slot names |

## Source map (`src/`)

| file | content |
|---|---|
| `game.*` | addresses, offsets, accessors (player, objects, camera, world-to-screen, RTTI class names) |
| `hooks.*` | hook install + signature check, hit-check override |
| `pad.*` | XInput (xinput1_4/1_3/9_1_0), dead zones, repeat, rumble, developer inject file |
| `control.*` | movement, camera, soft targeting, A / hold-A, quickslots, radial and action-target control |
| `ui.*` | frame hook and input routing between the screens below |
| `render.*`, `font.cpp`, `fonts_data.c` | overlay renderer, SDF font atlas (Cinzel, Lora embedded) |
| `uikit.*` | theme, panels, pad glyphs, hint bar |
| `hud.*` | gamepad HUD; hides the game's quickbar, party bar, action queue, state bar, chat, compass |
| `dialog.*` | conversation window |
| `hub.*` | Start menu frame, Inventory / Character / Quickbar tabs, page dispatch (`hub_pages.h`) |
| `hub_spells.cpp` | Spells page: class / level lists, cast, quickbar assign, prepare / clear slots |
| `hub_journal.cpp` | Journal page (asks the server for the quest list while open) |
| `hub_map.cpp` | Map page: tile icons, explored flags, hero / party / map notes |
| `hub_system.cpp` | System page: save / load lists, rest, option screens, exit |
| `describe.*`, `item_details.*` | silent examine (item description text) and the shared details pane |
| `twoda.*` | 2DA reader (spells.2da, classes.2da) through the resource manager |
| `inv.*` | inventory / character data and actions (equip, unequip, examine, drop, take, item icons incl. PLT armour) |
| `loot.*` | container window (chests, corpses) |
| `store.*` | merchant window |
| `quickbar.*` | quickbar names, icons, assign radial |
| `navcursor.*` | gamepad cursor for every other game panel (front end, level-up, spell book, journal, options ...) |
| `res.*`, `text.*`, `gamedata.*` | resources (TGA/DDS/PLT), TLK strings, code page conversion, creature data |

Input routing per frame (`Control_UIWantsInput`): dialogue -> game panels needing the cursor ->
merchant -> loot -> hub -> gameplay. The radial menu and the targeting mode always go to the
gameplay layer (`control.cpp`), also when opened from the hub.

## Reverse engineering

Ghidra 11.4.2 headless on both `nwmain.exe` and the non-stripped Linux `nwmain` of the same
build; ~13.7k functions matched across builds (exports, tracy source locations, strings,
call-graph propagation). Every function the mod calls was checked by decompiling both sides.
Rules of thumb: `CClientExoAppInternal`/`CNWCCreatureStats` offsets match Linux; most other
client classes are Linux-0x20 on Windows; Windows vtable offsets are Linux-8.
All addresses and structures are in `docs/RE_NOTES.md`.

## Testing (what was done)

Wine 9 + Xvfb + llvmpipe, game launched with `+TestNewModule "Prelude"` /
`"Contest Of Champions 0492"`. The `[Developer] InjectFile` setting makes the pad read
`buttons lx ly rx ry lt rt` from a text file every frame; `<InjectFile>.cmd` accepts
`target <name>` (lock the soft target), `use <name>` (default action on an object),
`ev <hex>` (fire a game input event), `journal` (log quest counts) and `probe` (log screen
projection). `tools/wine-test/regress.sh` runs the end-to-end checks. Console cheats
(`DebugMode 1`, `dm_spawncreature`, ...) were typed with xdotool. Verified: movement, camera,
targeting, interact, hold-A radial and wheel selection, quickslots, spell targeting confirm,
conversation, hub tabs, equip / unequip, item radial, examine, quickbar assign radial,
spells page (bard: cast, quickbar assign; wizard: cast, prepare, clear), journal (Chapter 1
quest), map (Prelude / Chapter 1 areas), save to a new slot, save list with previews,
item descriptions in inventory / loot / merchant,
container loot / take / close, merchant buy / sell / tabs / pages / close, game panels with the
cursor (main menu, options, character sheet, spell book, journal, map), rest.

Not covered in Wine: real XInput hardware, rumble, multiplayer servers, DM client,
hostile-creature melee (attack goes through the game's own default action / radial).
Loading a save did nothing in the Wine test setup, also through the game's own Load panel;
the Load list sends exactly the same request as that panel.

## Ideas for next steps

* Metamagic preparation and domain slot changes on the Spells page (done in the game's
  memorization panel today).
* Typing a save name (an on-screen keyboard); new saves are named after the area and time.
* Level-up and character creation screens on the pad UI.
* Quick item wheel (potions / scrolls) on a long press.
* Split / merge stacks and container bags inside the inventory screen.
