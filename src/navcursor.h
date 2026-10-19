#pragma once
void Cursor_Init();
bool Cursor_Active();
bool Cursor_GameNeedsCursor(bool dialogHandled);
void Cursor_Update(bool active);
void Cursor_Draw();
void Cursor_SetMouseVisible(bool vis);
void Cursor_InstallHooks();
