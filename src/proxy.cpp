// version.dll proxy: forwards every export to the system version.dll
#include <windows.h>
static HMODULE g_real = nullptr;
static FARPROC g_fn[17];
static const char* g_names[17] = {
 "GetFileVersionInfoA","GetFileVersionInfoByHandle","GetFileVersionInfoExA","GetFileVersionInfoExW",
 "GetFileVersionInfoSizeA","GetFileVersionInfoSizeExA","GetFileVersionInfoSizeExW","GetFileVersionInfoSizeW",
 "GetFileVersionInfoW","VerFindFileA","VerFindFileW","VerInstallFileA","VerInstallFileW",
 "VerLanguageNameA","VerLanguageNameW","VerQueryValueA","VerQueryValueW"};
void Proxy_Load() {
    if (g_real) return;
    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\version.dll");
    g_real = LoadLibraryA(path);
    for (int i = 0; i < 17; i++) g_fn[i] = g_real ? GetProcAddress(g_real, g_names[i]) : nullptr;
}
#define FWD(i, name) extern "C" __attribute__((naked)) void proxy_##name() { \
    __asm__ __volatile__("jmp *%0" :: "m"(g_fn[i])); }
FWD(0, GetFileVersionInfoA) FWD(1, GetFileVersionInfoByHandle) FWD(2, GetFileVersionInfoExA)
FWD(3, GetFileVersionInfoExW) FWD(4, GetFileVersionInfoSizeA) FWD(5, GetFileVersionInfoSizeExA)
FWD(6, GetFileVersionInfoSizeExW) FWD(7, GetFileVersionInfoSizeW) FWD(8, GetFileVersionInfoW)
FWD(9, VerFindFileA) FWD(10, VerFindFileW) FWD(11, VerInstallFileA) FWD(12, VerInstallFileW)
FWD(13, VerLanguageNameA) FWD(14, VerLanguageNameW) FWD(15, VerQueryValueA) FWD(16, VerQueryValueW)
