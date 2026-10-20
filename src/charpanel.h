// NWNPad - level-up and character creation screens on the pad UI
#pragma once
void CharPanel_Init();
void CharPanel_Frame();            // once per frame before input
bool CharPanel_Active();
bool CharPanel_Handled(void* panel);
bool CharPanel_Input();
void CharPanel_Draw();
