// NWNPad - item details pane (icon, name, type, examine text) shared by the loot and store windows
#pragma once
#include "inv.h"
#include <string>
struct ItemDetailsState { uint32_t id = 0; float scroll = 0; };
// extra: optional coloured fact line (price, "Unidentified" ...)
void UI_ItemDetails(const InvItem& it, float x, float y, float w, float h, float a, ItemDetailsState& st, const std::string& extra = std::string());
