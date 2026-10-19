// NWNPad - item descriptions (silent examine)
#pragma once
#include <string>
#include <cstdint>
void Describe_InstallHooks();
// description text of an item, or nullptr while it is being fetched from the server
const std::string* Describe_Get(uint32_t itemId);
void Describe_Forget(uint32_t itemId);
void Describe_Clear();
