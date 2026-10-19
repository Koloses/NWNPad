#pragma once
#include <string>
#include <cstdint>
// game (ANSI codepage) -> UTF-8
std::string G2U(const char* s, int len = -1);
// strip NWN colour tokens (<cRGB> ... </c>) and convert
std::string G2UClean(const char* s, int len = -1);
// read a CExoString (char* at +0, length at +8)
std::string ExoStr(const void* exoString);
int Text_CodePage();
