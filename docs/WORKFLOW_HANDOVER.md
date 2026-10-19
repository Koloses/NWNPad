# Handover: how NWNPad was made (workflow for the next game, NWN2)

Written for the next Claude session. It covers the method, tooling, traps and the user's
expectations, so the Neverwinter Nights 2 mod can be built the same way without
rediscovering everything. Read it fully before starting.

## 1. The user and the job

* User: Krzysztof (Poland, plays the Polish game language; Windows PC). Series of XInput pad
  mods: DSPad, DS2Pad, SacredPad, Sacred2Pad, SSPad, NWNPad. Next: **NWN2Pad** (name follows
  the series).
* The task is always the same: reverse engineer movement / interaction / combat / interface
  with Ghidra, then implement **modern twin-stick ARPG controls** + a **redesigned gamepad
  interface** (HUD, dialogue, menus). Pad must control hero, interactions, combat and UI.
* He does **not** test intermediate builds. Work autonomously until it works; test yourself.
* Style rules (strict):
  * Short explanations. Never use the phrase "smoking gun".
  * Never suggest stopping because of context, never suggest continuing in another session.
  * Do what is asked, no offers instead of doing.
* Deliverables: git repo (clean commits, feature-folded, no WIP, no build numbers in messages),
  release zip, sources zip, `README.md`, `HANDOVER.md`, `docs/RE_NOTES.md`, default ini.
  Copy them to his machine (device bridge): mod files into the game exe folder, zips + README
  into `<game>\<ModName>\`.
* Visual style: no fixed palette. The redesigned UI has to fit each game's own art and mood
  (NWNPad's dark panels with gold accents were chosen to match NWN:EE, not a series rule).
  Study the game's UI art, fonts and colours first and derive the theme from them; keep it
  modern and clean. The new HUD must sit behind an ini switch and fall back to the game's own
  HUD when off (and when no pad is used).

## 2. Environment (cloud container, Linux)

* The game lives on his PC (device folder, e.g. `E:\Gry\...`). Only `device_list_dir`,
  `device_stage_files` (PC -> container `/mnt/user-data/uploads/...`) and `device_commit_files`
  (container `/mnt/user-data/outputs/...` -> PC) were available, no shell on the PC.
  Stage only what you need; the big data archives can be symlinked from the uploads folder.
* Ghidra 11.4.2 in `/home/claude/tools/ghidra_11.4.2_PUBLIC` (headless). `analyzeHeadless`
  was patched to honour `MAXMEM` from the environment. **Never run two Ghidra JVMs at once**
  (OOM). Full analysis of a 20 MB exe takes a long time: run it in the background once,
  then use `-noanalysis -readOnly` for every later script run.
* Cross-compiler: mingw-w64 (`x86_64-w64-mingw32-g++`; NWN2 is 32-bit -> use
  `i686-w64-mingw32-g++` and 32-bit MinHook). Python with `pefile`, `pyelftools`, `fontTools`,
  `Pillow`.
* Test runtime: Wine 9 + Xvfb `:99` + llvmpipe. Prefix must be `/root/wineprefix`
  (Wine refuses a prefix owned by another user). Kill the game with `wineserver -k`, never
  `pkill -f <exe>` (it kills your own shell). Screenshots: `DISPLAY=:99 import -window root x.png`,
  then crop / downscale with PIL before reading them (image budget per request is limited).

## 3. Reverse-engineering method

1. **Find symbols.** NWN:EE was easy: the Linux build is not stripped and the Windows exe
   exports ~18k mangled symbols. I imported both into one Ghidra project and matched
   functions Linux->Windows (`tools/re/match.py`, `match2.py`): exports first, then
   tracy source-location strings (`srcloc.py`), unique strings, call-graph propagation,
   fuzzy body similarity. 13.7k matches, pickle + jsonl dumps (`DumpFuncs.java`).
   **NWN2 has no Linux build and no exports**: expect MSVC RTTI only. Plan B for NWN2:
   * RTTI: class names from `vtable[-1]` -> CompleteObjectLocator -> TypeDescriptor
     (runtime `G::ClassName()` in `src/game.cpp` does exactly this; do the same in Ghidra
     with `DumpVtables.java` to name vtables and their methods).
   * Assert / log strings (file names, function names in error messages), UI XML names,
     script function names (NWScript action table maps names to handlers).
   * NWN2 shares design DNA with NWN1 (CNWS/CNWC naming, CExoString, CResRef, message
     protocol) — the NWN:EE names and structures in `RE_NOTES.md` are a strong guide for
     what to look for, but offsets and code are different. The community (NWNX4, Skywing's
     NWN2 client extension) documented parts of nwn2main/nwn2server; search for them.
   * NWN2's GUI is data driven (XML UIScene files in the game's UI folders). Reading those
     XML files tells you every screen, button name and callback — use it.
2. **Decompile on demand.** `dl.sh out.c regex...` (Linux), `dw.sh out.c 0xADDR...` (Win),
   `pair.sh tag regex...` both sides + match info, `clean.py` strips noise,
   `q.py regex` prints name -> Linux/Win address with the match reason, `callers.py`.
   Every function the mod calls was verified by reading both decompilations; fuzzy matches
   were wrong several times.
3. **Struct offsets.** Learn the rule of thumb per class (in EE: app-internal and stats
   identical, most client objects -0x20 on Windows, vtable slots -8 on MSVC) but confirm at
   runtime: log the fields, then change state in game (equip, unequip) and **diff a memory
   snapshot of the object** to see which field moved. That found the possessor, container and
   equipped fields in minutes.
4. **Prefer the game's own logic over re-implementing it.** The best tricks of this project:
   * Hook the world hit-test functions (FeedbackHitCheck*, GuiObjectHitCheck) and return the
     pad's soft target while calling the game's own LButtonDown / RButtonDown /
     action-target handlers -> attack, talk, use, radial menus and spell targeting all work
     with zero game-logic code.
   * Drive analog movement through the game's keyboard drive mode (set its fields, send
     its input events, set orientation from the stick).
   * Call the game's own `Perform*` functions and network `SendPlayerToServer*` messages for
     inventory/store/loot actions.
   * Get human-readable names by calling the game's tooltip builder with `ShowTooltipText`
     hooked to capture the string.
   * For any screen not redesigned: a gamepad cursor (left stick + d-pad snap to the next
     control found by hit-testing, A/X = mouse buttons sent straight to the GUI manager).
5. **Server-side gating.** The client only receives inventory / character data while it tells
   the server the panel is open (`Gui*_Status` messages). If data does not update, look for such
   a status message.

## 4. Mod architecture (copy it)

* Proxy DLL (`version.dll`, forwards exports) loaded by the exe; init only in the game process.
  For NWN2 check which system DLL it imports that is safe to proxy (d3d9.dll is common but
  clashes with ReShade / dxvk; version.dll or dinput8.dll are usually safer).
* `Hooks_Install` first compares 8 bytes at every used address with `src/sigs.inc`
  (generated by `tools_gen_sigs.py` from the exe) and disables itself on mismatch.
* MinHook. Overlay renderer hooked at present time (EE: `gdi32!SwapBuffers`, GL 3.3).
  **NWN2 is Direct3D 9**: hook `IDirect3DDevice9::EndScene`/`Present` and port `render.cpp`
  to D3D9 (SDF shapes + SDF text in a pixel shader, state block save/restore).
* Modules: pad (XInput, dead zones, repeat, rumble, inject file), control (gameplay),
  ui (routing), hud, dialog, hub (inventory/character/quickbar tabs), loot, store, navcursor,
  res (TGA/DDS/PLT loading through the game's resource manager), text (game code page ->
  UTF-8; Polish = cp1250).
* Input routing each frame: dialogue -> game panels needing the cursor -> store -> loot ->
  hub -> gameplay; radial / targeting always to gameplay.
* Fonts: pick open-licence fonts that suit the game (NWNPad: Cinzel + Lora), subset with Latin Extended (Polish), embedded.

## 5. Testing workflow (essential: he will not test)

* Launch straight into a module: EE used `nwmain.exe +TestNewModule "Prelude"`. Find NWN2's
  equivalent (command-line module load, or a quicksave + `-load` style switch) on day one.
* Virtual pad: ini `[Developer] InjectFile=Z:\tmp\pad.txt`; the DLL reads
  `buttons lx ly rx ry lt rt` every frame. `<InjectFile>.cmd` takes developer commands
  (`target <name>`, `probe`). Scripts in `tools/wine-test/`:
  `run.sh`, `pad.sh`, `tap.sh` (hold >= 0.25 s, the Wine frame rate is low), `shot.sh`,
  `lib.sh` (`start`, `waitlog`, `pressuntil`, `T`, `console`), `regress.sh`.
* Make the DLL log one line per user-level action (dialog select, equip, take, store buy,
  area changed ...). Tests wait for log lines instead of sleeping blindly.
* Typing into the game: xdotool `key` per character (`type` drops characters under Wine).
  NWN debug console (`` ` ``, `DebugMode 1`, `dm_*` commands) was useful for spawning creatures;
  NWN2 has a similar console (`DebugMode 1`, `rs` script commands) — check.
* Game data: the 70+ GB of archives cannot all be staged; stub the big ones (EE: `stubbif.py`
  writes empty BIFs) and stage only what the test area needs.
* Before every release run `regress.sh` end to end, then check the log for errors.

## 6. Traps hit in this project

* A fuzzy-matched function looked right and was wrong -> always decompile.
* Screen projection was y-up in GUI coordinates but y-down in the projection result: check
  the coordinate convention of every API with a probe.
* GUI buttons in the front end ignored clicks sent through the in-game input path; send
  clicks to the GUI manager directly. Do not move the mouse between press and release
  (buttons re-hit-test on move and cancel the click).
* NWN:EE mixes two UI systems: the classic GUI manager and Nuklear screens (module browser).
  Nuklear reads the OS mouse, so the pad cursor must click with real input (SetCursorPos +
  SendInput). Check every front-end screen with a real click path, not just one.
* Radial menus count as modal panels; action-target mode can coexist with a modal panel.
  Order the "needs cursor" checks carefully.
* `std::string` returned from game helpers: free game `CExoString`s with the game's own
  destructor; release `shared_ptr` control blocks by hand (MSVC layout).
* When the pad disconnects, give everything back to the game (HUD panels, dialog panel,
  mouse cursor), otherwise a mouse user is stuck.
* Keep the overlay off when a game panel needs the cursor, otherwise it covers the panel.

## 7. Suggested plan for NWN2Pad

1. Stage `nwn2main.exe` (+ any DLLs), check bitness, imports, RTTI; start full Ghidra
   analysis in the background. Stage the UI XML files and read them meanwhile.
2. Identify: main loop / input dispatch, player object, movement (NWN2 has click-to-move
   and WASD in later patches), camera, picking (mouse hit test), default action, radial /
   context menu, action-target mode, quickbar ("hotbar"), dialogue UI, inventory, store,
   container, examine.
3. Bring up the test harness and the proxy DLL with logging first, then movement, then the
   hit-test override, then the overlay renderer (D3D9), then screens.
4. Commit per feature, run the regression before each release, deliver as in section 1.
