// NWNPad - frame profiler: time spent in our code per frame, spikes logged ([Debug] Profile=1 for the
// 10 s summaries)
#pragma once
#include <cstdint>
enum ProfSec { PS_UI, PS_PREINPUT, PS_POSTINPUT, PS_DRIVE, PS_HOOKS, PS_HUD, PS_MAP, PS_GL, PS_QUEST, PS_COUNT };   // HUD..QUEST: parts of UI
void Prof_Add(ProfSec s, double ms);
void Prof_Frame();                         // once per frame (SwapBuffers)
double Prof_Now();                         // ms
struct ProfScope {
    ProfSec s; double t0;
    explicit ProfScope(ProfSec sec) : s(sec), t0(Prof_Now()) {}
    ~ProfScope() { Prof_Add(s, Prof_Now() - t0); }
};
