# NWN:EE (build 89.8193.37-17, 26c6e573, Oct 2025) – reverse-engineering notes
Win64 nwmain.exe image base 0x140000000. Linux nwmain-linux (not stripped) used as symbol reference.
Linux addresses below are ELF vaddr (Ghidra = +0x100000). Windows addresses are for 89.8193.37-17; the Linux ones are from the 89.8193.37-14 Linux build.

## Globals
- g_pAppManager (exported ?g_pAppManager@@3PEAVCAppManager@@EA). *g_pAppManager = CClientExoApp*; CClientExoApp+8 = CClientExoAppInternal*.

## CClientExoAppInternal (offsets identical Linux/Win)
- +0x30 CNWCModule*, +0x90 CGuiInGame*, +0x108 active CActionTarget*, +0x118 input ctx, +0x120/+0x124 mouse x/y
- +0x140 byte "mode" (1 = dragging/mouse-drive), +0x154 box select, +0x208 CNWCMessage*
- drive mode: +0x198 timer, +0x1a0 timer, +0x1a8, +0x1b0 dirty(force send), +0x1b4 fwd, +0x1b8 fwd2, +0x1bc back,
  +0x1c0 turnL, +0x1c4 turnR, +0x1c8 strafeL, +0x1cc strafeR
- DriveControl flags: bit0 fwd, bit1 back, 3 = run fwd, 4/8 strafe, 0x10/0x20 turn

| function | Linux | Win |
|---|---|---|
| ProcessInput | 0x665140 | 0x1406f14e0 |
| HandleInputEvent(id,pressed,prev,gui) | 0x663140 | 0x1406ed720 |
| UpdateDriveMode | 0x65db50 | 0x1406f1e40 |
| GetPlayerCreature | 0x63dcd0 | 0x1406d8ad0 |
| GetModule | 0x638c50 | 0x1406d8300 |
| GetGameObject(id) | 0x63d680 | 0x1406d81f0 |
| PerformLButtonDownAction | 0x6625c0 | 0x1406f02c0 |
| PerformDefaultActionOnObject(obj) | 0x65ec00 | 0x1406efec0 |
| FeedbackHitCheckHostile(obj**,pos*,int*,int) | 0x65ede0 | 0x1406eae20 |
| FeedbackHitCheckNonHostile | 0x65fcc0 | 0x1406ebcf0 |
| CClientExoApp::GetPlayerCreature | | 0x1406ac1a0 |

## CNWCCreature (Win offsets differ from Linux, typically -0x20 after base CNWCObject)
- +0x38 pos(x,y,z), +0x44 orientation vec, +0xc type, +0x8 object id (CNWCObject base same)
- drive precondition: Linux 0x39c&2 / Win 0x37c&2 ; Linux 0x43c / Win 0x41c

## Input event ids (HandleInputEvent)
0x57 strafeL, 0x58 strafeR, 0x59 forward, 0x5a fwd2, 0x5b back, 0x5c turnL, 0x5d turnR, 0x12 escape/cancel
Radial key ids: 0x2f..0x37 octants (see CRadialMenu::HandleKeyEvent), 0x33 centre, 0x12 cancel.

## vtable rule
GCC vtables have 2 dtor slots; MSVC 1 -> Win vfunc offset = Linux offset - 8 for slots after dtor.

## Function map (Win VA, all verified by decompiling both builds)
| function | Win |
|---|---|
| CClientExoAppInternal::ProcessInput (hooked: pad input before the game) | 0x1406f14e0 |
| CClientExoAppInternal::HandleInputEvent(this,id,value,prev,gui) | 0x1406ed720 |
| CClientExoAppInternal::UpdateDriveMode (hooked: analog movement) | 0x1406f1e40 |
| CClientExoAppInternal::UpdateMouseCursorAppearance (hooked: hover = soft target) | 0x1406f2830 |
| CClientExoAppInternal::FeedbackHitCheckHostile / NonHostile (hooked: world pick) | 0x1406eae20 / 0x1406ebcf0 |
| CClientExoAppInternal::PerformDefaultActionOnObject | 0x1406efec0 |
| CClientExoAppInternal::PerformL/RButtonDownAction | 0x1406f02c0 / 0x1406f10e0 |
| CGuiMan::GuiObjectHitCheck (hooked) / HandleKeyEvent / SetMousePosition | 0x1401f4e30 / 0x1401f5270 / 0x1401f6870 |
| Camera::GetYaw / MapToScreen(this, Vec3* out, Vec3* in, bool normalized) | 0x140059020 / 0x140059a90 |
| CGuiQuickButton::HandleLeftButton / HandleRightButton / HandleTriggerTooltip | 0x14085c8f0 / 0x14085e810 / 0x14085e9b0 |
| CGuiInGame::ShowDialogEntry / ShowDialogReplies / HandleDialogSelection | 0x1406fb560 / 0x1406fb8e0 / 0x1406f81e0 |
| CGuiInGame::AppendToMsgBuffer (hooked: HUD feed) | 0x1406f68d0 |
| CGuiActionTarget::HandleLMouseDown | 0x14083be00 |
| CClientExoApp::GetObjectName(app,id,CExoString*) | 0x1406abfc0 |
| CClientExoApp::GetNWCMessage / GetInventoryPanel / GetWorldStore | 0x1406abf90 / 0x1406abbe0 / 0x1406ac320 |
| CClientExoApp::GetPauseState(app, 2) | 0x1406ac030 |
| CNWCCreature::PerformEquipItem(cre,item,slotMask) / UnEquip(cre,item) | 0x1408050d0 / 0x140805690 |
| CNWCCreature::PerformDropItem(cre,item) / PerformExamine(cre,id,0,0) | 0x140804ed0 / 0x140805290 |
| CNWCItem::ActivateRadialMenu(item,int) / GetName(item,CExoString*,int) | 0x14083ce30 / 0x14083d8a0 |
| CNWCMessage::SendPlayerToServerInventory_RepositoryMove(msg,item,x,y,repo) | 0x1407c1030 |
| CNWCMessage::SendPlayerToServerGuiInventory_Status(msg,open,cre) | 0x1407be110 |
| CNWCMessage::SendPlayerToServerGuiCharacterSheet_Status(msg,page,cre) | 0x1407bddd0 |
| CNWCMessage::SendPlayerToServerStore_RequestBuy / RequestSell / Close | 0x1407c3c10 / 0x1407c3d00 / 0x1407c3a50 |
| CNWCMessage::SendPlayerToServerMessage(msg,major,minor,data,size) | 0x1407c25c0 |
| CPanelInventory::HandleXButton (closes container view) | 0x140856420 |
| CNWCStore::GetIsRestrictedBuyItem(store,baseItem) | 0x140841890 |
| CNWCCreatureStats: GetClassesAbilityAdjust / GetArmorClass / GetCombatModeACModifier / GetBaseAttackBonus | 0x1407f1110 / 0x1407ef9c0 / 0x1407f1ed0 / 0x1407f0900 |
| CNWCCreatureStats: GetClassesString / GetRaceString / GetExpNeededForLevelUp / GetLevel | 0x1407f1230 / 0x1407f67c0 / 0x1407f3da0 / 0x1407f5d70 |
Exported (by name): g_pAppManager, g_pGuiMan, g_pRules, g_pExoResMan, g_pTlkTable, CExoResMan::Get,
CTlkTable::GetSimpleString, CGuiPanel::Hide/Deactivate, CGuiMan::HandleL/RMouseDown/Up, HandleMouseWheel,
ShowTooltipText, ShowMouse/HideMouse, CNWBaseItemArray::GetBaseItem, CNWMessage::CreateWriteMessage/WriteBYTE/GetWriteMessage,
Encoding::g_DefaultLocale.

## Client object array
CClientExoAppInternal+0x28 -> CGameObjectArray*: buckets at +0, modulus +0x1c, bucket count +0x24;
node {u32 id; CGameObject* obj @+8; next @+0x10}. Item: CGameObject sub-object at CNWCItem+0x48 (vfunc AsNWCItem 0x68).

## CNWCItem (Win)
+0x40 base item, +0x50 object id, +0x1c0 identified, +0x1c4 stolen, +0x238 CGuiIcon*, +0x254/+0x255 repository x/y,
+0x25c equipped by, +0x260 pending-move flag, +0x264 possessor (inventory owner), +0x268 1 = in an open container view,
+0x280 base cost, +0x284 stack size, +0x290 CExoString name.
CNWBaseItem: +0 name strref, +4 EquipableSlots mask, +0x16 model type, +0x24 stacking.
Equipment slot ids (mask 1<<i): 0 head 1 chest 2 boots 3 arms 4 right hand 5 left hand 6 cloak 7/8 rings 9 neck 10 belt
11 arrows 12 bullets 13 bolts 14-17 creature slots. Equipped item ids at CNWCCreature+0x308+4*i. Gold at +0x1b8.
The server streams inventory contents only while the client reports the inventory GUI as open
(SendPlayerToServerGuiInventory_Status); repository updates are tagged with CPanelInventory+0x2f68 (char id).

## Item icons
CGuiIcon: resref +8 (16 chars), size +0x1c/+0x20/+0x24. CCompositeIcon (weapons): resrefs +8, +0x48, +0x59 (layers b/m/t).
CLayeredIcon: resref +8, colour count +0x48, colours (u16 palette<<8|row) +0x50.
CArmorIcon: 6 part resrefs at +0x48 step 0x11 (PLT), colour count +0xb0, colours +0xb8, per-part colours +0xc0 (6*count).
PLT = "PLT V1" header, w/h at +16/+20, pixels (value, layer) bottom-up; colour = pal_<layer>.tga[row][value].
Palettes: pal_skin01, pal_hair01, pal_armor01, pal_armor02, pal_cloth01, pal_leath01, pal_tattoo01.

## Containers / stores
CPanelInventory (Win): +0x2f64 container open, +0x268c container id, +0x2f68 character id. Container item take:
RepositoryMove(item, 0xff, 0xff, INVALID). Page next/prev: message 0x19/2 and 0x19/3.
CNWCStore (Win): +0x170 CPanelStore*, +0x178 open, +0x17c markdown, +0x17e markup (short), +0x180 stolen markdown,
+0x184 black market, +0x18c max buy price, +0x190 store gold (-1 unlimited).
CPanelStore (Win): +0xf0 CGuiMultiPageMultiInvbox (cols +0x190, rows +0x194, ids +0x1a0), +0x2c0 page, +0xb18 page count,
+0xb1c tab, +0xd34 tab name strrefs. Buy price = cost*markup/100, sell = cost*markdown/100 (capped by max buy).
Change tab: message 7/4 (byte tab); page: 7/5 (byte page). Closing: CGuiPanel::Deactivate(panel,0).

## CNWCCreatureStats (offsets same on both builds)
+0x50 race, +0x69 class count, +0x76..0x7b base STR DEX CON INT WIS CHA, +0x7d..0x82 bonuses, +0x86 temp HP, +0x88 HP,
+0x8a max HP, +0x8c natural AC base, +0x94 experience, +0xb4 portrait resref, classes at +0x295 + i*0x150 (id, level, neg).
Racial ability adjust: rules+0x118 races, stride 0x68, STR +0x18 DEX +0x19 INT +0x1a CHA +0x1b WIS +0x1c CON +0x1d.
XP table: rules+0x28 + level*4.

## Quick buttons
CGuiInGame+0x60 CPanelQuickBar; buttons at +0xf0 + (set*12+slot)*0x2c8; button type +0x140, icon resref +0x11c,
disabled flag +8 bit0. Names come from HandleTriggerTooltip with CGuiMan::ShowTooltipText hooked to capture the text.

## GUI
CGuiMan: +0x58 captured object (radial 'RMnu' 0x524d6e75, action target 'AcTg' 0x41635467), +0x78 hit cache,
+0x88/+0x90 modal panel stack, +0xb8/+0xbc GUI size, +0xd4/+0xd8 mouse (y up).
Front-end clicks must go through CGuiMan::HandleLMouseDown/Up (the in-game input path ignores them).
CGuiInGame (Win): +0x70 dialog panel, +0x78 char sheet, +0x60 quickbar, +0x58 party bar, +0x68 compass, +0xa8 action queue,
+0xb0 player state bar, +0x3a0 popup array, +0x3c4 popup count.

## Spells (Win)
CNWCCreatureStats: class info at +0x148 + i*0x150: known spells CExoArrayList<uint32>[10] at +0 (stride 0x10),
memorized CExoArrayList<CNWCStats_Spell*>[10] at +0xa0, spells-per-day left uint8[10] at +0x140, class id +0x14d, level +0x14e.
CNWCStats_Spell: id +0, ready +4, metamagic +8 (byte), domain +0x10.
Spell "menu id" (quickbar / radial): id bits 0-15, spell level 24-27, class index bits 28-29 + bit 38, metamagic 31-36,
bit 37 spontaneous conversion.
Cast: CGuiQuickButton::ButtonActionCastSpell 0x140859d30 reads only this+0x110 (menu id) -> game targeting mode.
Quickbar: CGuiQuickButton::SetButtonSpellMenuID 0x140868e70 (button, menu id).
Prepare: SendPlayerToServerSpellMemorize 0x1407c3870 (msg, class idx, spell, slot, domain level, meta) = message 6/0x10;
clear: message 6/0x11 bytes (class idx, level, slot). Send: CNWCMessage::SendPlayerToServerMessage 0x1407c25c0.
classes.2da SpellCaster / MemorizesSpells / SpellTableColumn; spells.2da Name / IconResRef / SpellDesc / SubRadSpell1-5.

## Journal (Win)
CClientExoAppInternal+0x70 CNWCJournal: done list +0, active list +0x28 (CNWCSortedJournalArrayList: entries +0,
count +8, sorted index array +0x10). SJournalEntry 0x50: name loc +0, text loc +0x10, day +0x20, time +0x24,
tag +0x28, priority +0x38, state +0x3c, picture +0x40, completed +0x44, displayed +0x48, updated +0x4c.
The server sends quests only while the quest screen is open: message 0x1c/10 open, 0x1c/11 closed.
Text: CExoLocString::GetString(lang = internal+0x200, out, gender 0, true) (export).

## Map (Win)
CNWCArea: width +0xc, height +0x10 (tiles of 10 m), name CExoString +0x238, tiles +0x298 (0x60 each),
map pins +0x2b8 / count +0x2c0. CNWCTile: orientation +0x24 (quarter turns), explored +0x30, tile data +0x40
(map icon resref at +0x21). CNWCMapPin: object position +0x38, enabled +0x170, note CExoString +0x178.

## Save / load (Win)
CSaveGameList ctor 0x140814a50 (0x30 bytes: dir names CExoString[] +0, CFileInfo[] +0x10 (0x20 each: file name,
year +0x10 month +0x12 day +0x14 hour +0x16 minute +0x18), order int[] +0x20 / count +0x28). Dir = "NNNNNN - name".
Save: internal 0x1406df2a0 (internal, number, name, module name (module+0x18), overwrite dir or "") = message 3/4.
Load: internal 0x1406df280 (internal, number, name, .sav file name without extension, 0) = message 3/5.
COptionsPanel (GIG+0xd8, toggled by CGuiInGame::HandleOptionsButton 0x1406f86c0): button callbacks {obj, fn} at
Load +0x240, Save +0x328, SaveChar +0x410, Video +0x4f8, Controls +0x6c8, Exit +0x7b0, Resume +0x898, Game +0x980,
X +0xa68; the sound button is removed in EE.

## Item descriptions
Examine request: CNWCMessage::SendPlayerToServerInput_Examine 0x1407bfa80 (msg, id); reply handled by
HandleServerToPlayerExamineGui_ItemData 0x14079a1e0, which fills StringGob texts (SetText / AddText / AddLine
exports) of a new examine window; capture the outermost calls and close the window with CGuiModalPanel::Deactivate.

## Inventory actions (Win)
Split stack 0x1407c0050 (msg, id, count) = message 6/0x17; merge 0x1407bfd90 (msg, into, from) = 6/0x16;
open a bag = UseObject 0x1407c07c0 (msg, id, 0, 0); put into a bag = RepositoryMove 0x1407c1030 (msg, id, 0xff, 0xff, bag).
baseitems.2da Container / Stacking columns.

## Party (Win)
CNWCCreature::PerformQuickChat 0x140805440 (creature, voice id): follow 22, guard 9, attack 0, stand ground 10,
heal me 4. Associate command 0x1407bf0e0 (msg, associate, cmd): party stealth -22, search -23.
Possess familiar / return 0x1407bff40 (msg). Controlled associate type 0x1406ac1b0 (app) (3 = familiar);
creature associate type +0x274.

## Metamagic and domains
Feats: Empower 11 -> metamagic bit 1 (+2 levels), Extend 12 -> 2 (+1), Maximize 25 -> 4 (+3), Quicken 29 -> 8 (+4),
Silent 33 -> 16 (+1), Still 37 -> 32 (+1). spells.2da MetaMagic column = allowed bits. Feats list stats+0x118 / count +0x120.
Domain ids: class info +0x14b / +0x14c; domains.2da Level_N = domain spell of level N.

## Classic panels (generic access)
CGuiObject::ImportLayoutData 0x1401fe0b0 (object, panel, layout name) names every control; CGuiPanel::Activate
0x1401fbe70 / Deactivate 0x1401fc470; ?Hide@CGuiPanel / ?UnHide@CGuiPanel exports. CGuiObject flags +8 (bit0 disabled);
CGuiPanel +0xb0 (bit0 active, bit1 hidden). Callbacks: CGuiButton obj +0x90 / fn +0x98; check / radio box obj +0x80 /
fn +0x88 (int checked, state +0x90); CGuiTimerButton +0xd0 / +0xd8; tab buttons: index +0xa0, tab set +0xa8
(array +0x18 / count +0x20, selected +0x10), click with ?HandleTabClick@CGuiTabSet@@IEAAXH@Z.
?s_pCurrentEventObject@CGuiObject@@1PEAV1@EA must point to the clicked control.
CGuiButtonListbox: selected +0x90, rows +0x98 / count +0xa0, ids +0x420 / count +0x428. CGuiClassButton enabled +0xc8.
Skill rows: timer up / down buttons; feat / spell rows: CGuiMouseoverChildButton add-remove arrow then info.

## Level-up and character creation (Win)
Level-up mode: CGuiInGame+0x2c. Start level-up: 0x1406f85b0 (CGuiInGame::HandleLevelUpButton).
Panels (layout names in brackets): CCharacterClassPanel (ClassListBox), CCharacterLevelUpAbilitiesPanel /
CCharacterAbilitiesPanel (StrLabel/StrEdit/StrUpButton/StrDownButton ..., PointsEdit), CCharacterSkillsPanel
(SkillsButtonBox), CCharacterFeatsPanel / CCharacterSpellsPanel (AvailBox, KnownBox, SpellLevel0..9),
CCharacterSchoolsPanel, CCharacterDomainPanel, CCharacterFamiliarPanel (FamListBox, NameEditBox, GenNameButton),
CCharacterLevelUpSummaryPanel (EditBox), CCharacterSexPanel / CCharacterAlignmentPanel (radio buttons),
CCharacterRacePanel, CCharacterPackagesPanel (ClassListBox, ConfigurePckg), CCharacterInfoPanel (FirstNameEdit,
LastNameEdit, SoundSetEdit, RandomButton). Common: OkButton, CancelButton, RecommendButton, ResetButton, HelpBox, Title.
CCharacterGenerationPanel: step buttons GenderButton RaceButton PortraitButton ClassButton AlignButton AbilitiesButton
PackagesButton CustomizeButton PlayButton (CGuiCharGenCompleteableButton, completed int +0xe8), encouraged button +0xc08,
creature id +0xbe8, char sheet panel +0x1a0.
CCharacterPortraitPanel: creature id +0x370, CGuiPortraitSelect +0x378 (resrefs 17 bytes at +0x408 / count +0x410,
tab set +0xf0); textures <resref>h/l/m/s, art fills the top 100/128 of the texture.
Creature stats: gender +0x68, law/chaos +0xc6, good/evil +0xc8 (short, >= 70 lawful / good, <= 30 chaotic / evil).

## Pop-ups
Hook ?Activate@CGuiModalPanel@@QEAAXXZ; classes CPanelOk, CPanelOkCancel, CPanelOkScroll, CPanelYesNoCancel.
Buttons: exported OnOkButtonPressed / OnCancelButtonPressed / HandleYes/No/CancelButton.
Game state icons over the portrait (level up 1, time stop 2, pause 4): CGuiInGame+0x370 bits;
AddGameState 0x1406f63d0 (gig, mask), RemoveGameState 0x1406fa410 (gig, mask).

## Health of other creatures
Exact HP (update flag 0x400 -> stats +0x84..0x8a) is sent only for creatures in the player's party
(CNWSMessage::ComputeUpdateRequired); everyone gets the damage level (flag 0x800 -> stats +0x8e byte:
0 >= 95%, 1 >= 75%, 2 >= 50%, 3 >= 25%, 4 alive, 5 dead; CNWSObject::GetDamageLevel). Non-party stats show 1/1.

## Walking to a point
CClientExoAppInternal::WalkPlayerToPoint 0x1406f3650 (internal, x, y, z, flags 0, target 0x7f000000, 0).
