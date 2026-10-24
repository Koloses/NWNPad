# NWNPad

Xbox / XInput gamepad mod for **Neverwinter Nights: Enhanced Edition** (Windows, build 8193.37).
It plays like a modern twin-stick action RPG and replaces the mouse-driven interface with
gamepad screens: HUD with minimap, conversation window, hub menu (inventory, character,
quickbar, spells, journal, map, system), loot window, merchant, party orders, game pop-ups,
character creation and level-up. Every other game panel (main menu, appearance, options ...)
gets a gamepad cursor that jumps between controls.

Everything was built from a decompilation of the game executable; no game files are changed.

NWNPad is only the gamepad and interface mod; it does not change how the game renders. To run the game's
OpenGL on Vulkan instead, see the separate NWNZink package.

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
| Left stick click | Party wheel (orders for henchmen, companions, summons, familiar) |
| Start | Hub menu |
| Back | Map (hub Map tab) |

The HUD has a minimap in the top-right corner that turns with the camera (what is ahead of the
camera is up, an N on the rim shows north): explored area, your position and facing, party (green),
enemies (red), other people and map notes. Back opens the full map.

The hero's portrait in the bottom-left corner is a rectangular card with the health bar next to it
(no ring around the hero in the world). Quick slots holding a mode (Power Attack, Expertise,
Rapid Shot, Flurry of Blows, Detect / Stealth, Parry, Defensive Casting, Dirty Fighting, Defensive
Stance ...) show its state: a turning arc while it switches, a gold ring and an ON badge while it
is active. Resting shows a progress card (time left); B stops resting.

The target is picked automatically from what is in front of the hero; enemies are preferred.
The target frame shows exact health for your party; for everyone else the game only tells the
client a damage level (uninjured ... near death), shown as the bar and its name.

**While paused** (d-pad right) the left stick moves a pointer over the world, like the mouse:
point at a creature or object and press A to act on it (hold A for its action wheel), or point
at the ground and press A to walk there. RB / LB jump the pointer between targets. Spells and
items that need a target use the pointer too. The orders run when you resume.

**Action wheels:** point with either stick, A selects, Y goes back one level, B closes.

**Party wheel** (left stick click): Follow me, Guard me, Attack, Hold position, Heal me,
Stealth and Search (whole party), Possess familiar / Return to master. Point, A orders,
B or left stick click closes.

**Level up:** when enough experience is gathered the HUD shows "Level up ready"; Start >
Character > X starts it.

**Spells and items that need a target:** the left stick moves a pointer over the world; A uses it
on the creature / object under the pointer (or on the ground there), RB / LB change the target,
Y targets yourself, B cancels. Used from a quick slot with a target selected, the item / spell goes
straight to that target.

**Giving items:** item menu > Give to ... : the left stick pointer (or RB / LB) picks a creature, A
gives. As with dragging the item onto them: your henchmen / companions drink a potion or use a
healer's kit right away, anyone else gets a barter offer.

**Quest marker (single player):** the minimap points at who moves your quest on: an orange marker
(or an arrow on the rim) for someone in this area, a blue one for the door / transition towards the
area they are in; the quest's name is under the map. It follows the quest updated last unless you
follow one: X on the hub Journal page or A on the hub Map page. The same button on the followed
quest unfollows it: no marker until one of your quests gets a new journal entry. The game has no quest markers:
the mod looks through the module's conversations for lines that set a later journal entry of an
active quest, so a script-only step (killing something, picking up an item) has no marker.
`QuestMarker=0` in `[Interface]` turns it off.

**Corpses** are no longer targeted (their loot is in the bag they drop); the pause pointer still
reaches them.

**Using items from the inventory:** Use (X > Use) drinks a potion straight away. Wands, scrolls,
kits and other items with one power go to target mode; items with several powers open the
game's item wheel.

### Hub (Start)

LB / RB switch tabs: Inventory, Character, Quickbar, Spells, Journal, Map, System.

* **Inventory** - equipment on the left, backpack in the middle, details on the right. The
  backpack is a grid like the game's (items take their real size, stack counts in the corner);
  Back switches to a list and back (`InventoryView=grid|list` in `[Interface]` sets the start).
  A on an equipped slot unequips it; A on an empty slot lists the backpack items that fit it.
  A on a backpack item equips it (or uses it; A on a bag opens it). X opens the item menu:
  equip, use, split stack (d-pad sets the amount, LT / RT min / max), combine stacks, open,
  put in a bag, drop, examine, more actions (the game's wheel). Y examines, left stick click
  cycles the backpack order (as carried, name, type, value, weight). Items your character
  cannot use (class, race, alignment, feats, ability scores) have a red background, the details
  say so, and equipping them is refused.
* **Character** - abilities, hit points, armour class, attack bonus, experience, gold.
  A opens the full character sheet, X levels up when available, Y rests.
* **Quickbar** - all 36 slots with the button that triggers each one. A opens the game's
  full assign wheel for the slot, also on a filled slot (attack, special attacks, abilities, cast,
  party commands, actions, emotes; the slot keeps its content if nothing is picked), X uses the
  slot, Y makes the set active, Back clears it.
* **Spells** - spells of each casting class by level (LT / RT or d-pad left / right change the
  level, Y the class). A casts (then pick the target as usual), X puts the spell on the
  quickbar (choose the slot, A places it). Prepared casters (wizard, cleric ...) press View to
  prepare: A on a slot picks a spell (metamagic versions, e.g. Empowered, appear when you
  have the feat and a slot of the right level; clerics also fill their domain slots), X clears
  it; new preparations are ready after resting.
  The right pane shows the spell's description (right stick scrolls).
* **Journal** - active and completed quests (X switches), the newest entry of the selected
  quest on the right.
* **Map** - explored part of the current area with your position and facing, party members,
  map notes with their names above the dots, and quest markers (diamonds with the quest's name:
  orange for someone here, blue "(exit)" for the way to another area; the followed one pulses).
  The list on the right has the quests and the notes: d-pad selects (the map shows it), A follows
  / unfollows a quest. Left stick pans, LT / RT zoom, Y re-centres.
* **System** - resume, quick save, save game (new save or overwrite, A twice to confirm),
  load game (list with previews), rest, save character, the game's option screens and exit
  to the main menu.

Item descriptions (the examine text) are shown in the details pane of the inventory, loot
and merchant windows; the right stick scrolls them.

### Conversations

D-pad / left stick choose a reply, A answers (or continues), B leaves. The speaker's full portrait
is shown next to the text.

### Loot (chests, corpses, bags)

D-pad choose, A takes the item, Y takes everything, X examines, LB / RB change the page,
B closes. The selected item's description is shown on the right.

### Merchants

Left list is the merchant, right list your backpack (d-pad left / right switches). A buys or
sells, Y examines, LB / RB change the category, LT / RT the page, B leaves. Prices the
merchant will pay are shown next to your items; red prices are out of reach. Items the merchant
will not take are dimmed with the reason instead of a price (plot item, stolen, cursed, kind of
item the merchant does not trade in, worthless, bags that are not empty). After a sale the mod
checks the item really left your backpack and says so if the merchant refused it.

### Character creation and level-up

Character creation (New Character on the character list) shows the steps in order with the
character so far: gender, race, portrait, class, alignment, abilities, package, customize,
play. A opens a step, Start plays when everything is done, View resets, B cancels.

In every step and in the level-up screens: d-pad / left stick choose, A picks (single choice)
or raises / adds, X lowers / removes, LB / RB switch between lists (available / chosen),
LT / RT change the spell level, Y takes the recommended choices, Start confirms, B goes back,
the right stick scrolls the description. The package step has X for Configure (choose skills,
feats and spells yourself). Portraits are a grid (LB / RB page). Appearance uses the gamepad
cursor (it needs the 3D preview); on the name and voice step View generates a name and X
switches to the game's panel to type a name or description.

### Pop-ups

Game messages (OK, OK / Cancel, Yes / No / Cancel) are shown as gamepad dialogs: A confirms,
X answers No, B cancels.

### Game panels (gamepad cursor)

Left stick moves the cursor, the d-pad jumps to the next control, A clicks, X right-clicks,
B goes back, the right stick scrolls lists.

## Settings

`NWNPad.ini` (comments inside): turn the new HUD, conversation window or menus off one by one
to fall back to the game's own panels, adjust dead zones, camera speed and inversion,
target ranges, UI scale and rumble.

## OpenGL diagnostics

`[Developer] GLDiag=1` in `NWNPad.ini` writes `bin\win32\NWNPad-diag\NWNPad-gl.log` (GPU, driver, every
shader compile / link with errors, driver warnings) and works even with `Enabled=0`. F11 (or both
stick clicks held for a second) captures the next frame into `NWNPad-diag\capture-<time>`: screenshot,
every draw call with its shader program, textures and uniform values, the shader sources and the
textures as TGA files.

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
