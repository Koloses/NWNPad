# NWNPad

Xbox / XInput gamepad mod for **Neverwinter Nights: Enhanced Edition** (Windows, build 8193.37).
It plays like a modern twin-stick action RPG and replaces the mouse-driven interface with
gamepad screens: HUD, conversation window, hub menu (inventory, character, quickbar), loot
window and merchant. Every other game panel (main menu, character creation, level-up, spell
book, journal, map, options) gets a gamepad cursor that jumps between controls.

Everything was built from a decompilation of the game executable; no game files are changed.

## Install

1. Copy `version.dll` and `NWNPad.ini` into `Neverwinter Nights - Enhanced Edition\bin\win32`
   (next to `nwmain.exe`).
2. Start the game normally. A log is written to `bin\win32\NWNPad.log`.

To uninstall delete the two files. If the game is updated to a different build the mod
detects it and disables itself (see the log).

## Controls

### Exploring and combat

| Input | Action |
|---|---|
| Left stick | Move (camera relative; light push walks, full push runs) |
| Right stick | Turn / zoom the camera |
| LB (hold) + right stick | Tilt the camera |
| A | Act on the highlighted target: talk, open, attack, pick up, use |
| A (hold) | Action wheel of the target (all radial actions); with no target: your own wheel |
| X / Y / B | Quickbar slots 1-3 |
| LT + X / Y / B / A | Quickbar slots 4-7 |
| RT + X / Y / B / A | Quickbar slots 8-11 |
| LT + RT + A | Quickbar slot 12 |
| D-pad up / down | Switch quickbar set (I, II, III) |
| D-pad left | Your own action wheel (feats, spells, modes) |
| D-pad right | Pause |
| RB / LB (tap) | Next / previous target (locks it) |
| Right stick click | Release the target lock |
| Start | Hub menu |
| Back | Map (hub Map tab) |

The HUD has a minimap in the top-right corner (north up): explored area, your position and
facing, party (green), enemies (red), other people and map notes. Back opens the full map.

The target is picked automatically from what is in front of the hero; enemies are preferred.

**Action wheels:** point with either stick, A selects, Y goes back one level, B closes.

**Spells and items that need a target:** A uses it on the highlighted target (or on the ground
ahead of the hero when nothing is highlighted), RB / LB change the target, Y targets yourself,
B cancels.

### Hub (Start)

LB / RB switch tabs: Inventory, Character, Quickbar, Spells, Journal, Map, System.

* **Inventory** - equipment on the left, backpack in the middle, details on the right.
  A on an equipped slot unequips it; A on an empty slot lists the backpack items that fit it.
  A on a backpack item equips it (or uses it). X opens the item's action wheel (use, drop,
  split ...), Y examines it.
* **Character** - abilities, hit points, armour class, attack bonus, experience, gold.
  A opens the full character sheet (level-up), Y rests.
* **Quickbar** - all 36 slots with the button that triggers each one. A opens the game's
  assign wheel for the slot (spells, feats, skills, items, commands), X uses the slot,
  Y makes the set active.
* **Spells** - spells of each casting class by level (LT / RT or d-pad left / right change the
  level, Y the class). A casts (then pick the target as usual), X puts the spell on the
  quickbar (choose the slot, A places it). Prepared casters (wizard, cleric ...) press View to
  prepare: A on a slot picks a spell, X clears it; new preparations are ready after resting.
  The right pane shows the spell's description (right stick scrolls).
* **Journal** - active and completed quests (X switches), the newest entry of the selected
  quest on the right.
* **Map** - explored part of the current area with your position and facing, party members
  and map notes. Left stick pans, LT / RT zoom, d-pad jumps between notes, Y re-centres.
* **System** - resume, quick save, save game (new save or overwrite, A twice to confirm),
  load game (list with previews), rest, save character, the game's option screens and exit
  to the main menu.

Item descriptions (the examine text) are shown in the details pane of the inventory, loot
and merchant windows; the right stick scrolls them.

### Conversations

D-pad / left stick choose a reply, A answers (or continues), B leaves.

### Loot (chests, corpses, bags)

D-pad choose, A takes the item, Y takes everything, X examines, LB / RB change the page,
B closes. The selected item's description is shown on the right.

### Merchants

Left list is the merchant, right list your backpack (d-pad left / right switches). A buys or
sells, Y examines, LB / RB change the category, LT / RT the page, B leaves. Prices the
merchant will pay are shown next to your items; red prices are out of reach.

### Game panels (gamepad cursor)

Left stick moves the cursor, the d-pad jumps to the next control, A clicks, X right-clicks,
B goes back, the right stick scrolls lists.

## Settings

`NWNPad.ini` (comments inside): turn the new HUD, conversation window or menus off one by one
to fall back to the game's own panels, adjust dead zones, camera speed and inversion,
target ranges, UI scale and rumble.

## Building

Linux with mingw-w64 (`x86_64-w64-mingw32-g++`), Python 3 with `pefile`:

```
make GAMEEXE=/path/to/nwmain.exe
```

`tools_gen_sigs.py` reads the first bytes of every game function the mod calls from the
supported `nwmain.exe` so the DLL can verify the build at start-up. Fonts are embedded from
`fonts/` (`tools_embed_fonts.py`). See `HANDOVER.md` and `docs/RE_NOTES.md` for the internals.

## Credits

* MinHook (BSD 2-clause), stb_image / stb_truetype (public domain / MIT)
* Cinzel and Lora fonts (SIL Open Font License, `fonts/OFL.txt`)
