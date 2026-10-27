#include "log.h"
#include <windows.h>
#include <cstdio>
static FILE* g_log = nullptr;
static CRITICAL_SECTION g_cs;
static ULONGLONG g_flushed = 0;
static volatile bool g_pending = false;
void Log_Init(const char* path) {
    InitializeCriticalSection(&g_cs);
    g_log = fopen(path, "w");
    if (g_log) setvbuf(g_log, nullptr, _IOFBF, 1 << 16);
}
void Log(const char* fmt, ...) {
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_log, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log);
    // written out at most once a second: a flush per line is a disk write per line on the game's thread
    ULONGLONG now = GetTickCount64();
    if (now - g_flushed >= 1000) { fflush(g_log); g_flushed = now; g_pending = false; }
    else g_pending = true;
    LeaveCriticalSection(&g_cs);
}
void Log_Tick() {
    if (!g_log || !g_pending || GetTickCount64() - g_flushed < 1000) return;
    EnterCriticalSection(&g_cs);
    fflush(g_log); g_flushed = GetTickCount64(); g_pending = false;
    LeaveCriticalSection(&g_cs);
}
