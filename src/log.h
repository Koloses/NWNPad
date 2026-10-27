#pragma once
#include <cstdarg>
void Log_Init(const char* path);
void Log(const char* fmt, ...);
void Log_Tick();                 // once per frame: writes out what waited more than a second
