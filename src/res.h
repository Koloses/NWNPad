#pragma once
#include <vector>
#include <string>
#include <cstdint>
void Res_Init();
bool Res_Load(const char* resref, unsigned short type, std::vector<uint8_t>& out);
uint32_t Res_Texture(const char* resref, int* w = nullptr, int* h = nullptr);   // GL texture (cached), 0 if missing
std::string Tlk(uint32_t strref);
// composite of PLT layers coloured with the game palettes; colors = 10 per part ((palette<<8)|row)
uint32_t Res_PltTexture(const std::vector<std::string>& parts, const std::vector<uint16_t>& colors, int* w = nullptr, int* h = nullptr);
