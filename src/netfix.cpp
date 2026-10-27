// NWNPad - the game looks up its master server (nwmaster2.beamdog.net) on its main thread every 60 s,
// single player included (the local server "posts the game to the internet" and the client lists it).
// When the DNS server doesn't answer, each lookup blocks until Windows gives up - about 12 s with the
// whole game frozen (nwengineLog: "Resolving masterserver DNS took longer than 3 seconds").
//
// The master server lookups are answered without waiting: a background thread does the real
// lookup, and the game's call goes through only once that succeeded recently (Windows' own cache then
// answers at once). Until then - and for a minute after a failure - the game is told "try again later",
// which it already handles (it retries in 60 s). Other names are untouched.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include "netfix.h"
#include "log.h"
#include "MinHook.h"
#include <string>
#include <map>
#include <mutex>
#include <cctype>

namespace {
typedef int (WSAAPI* getaddrinfo_t)(PCSTR, PCSTR, const ADDRINFOA*, PADDRINFOA*);
typedef hostent* (WSAAPI* gethostbyname_t)(const char*);
getaddrinfo_t o_getaddrinfo = nullptr, real_getaddrinfo = nullptr;
gethostbyname_t o_gethostbyname = nullptr;
decltype(&freeaddrinfo) p_freeaddrinfo = nullptr;
decltype(&WSASetLastError) p_WSASetLastError = nullptr;

struct Host { bool busy = false; bool ok = false; ULONGLONG at = 0; };   // at: when the last lookup ended
std::map<std::string, Host> g_hosts;
std::mutex g_mx;
const ULONGLONG OkFor = 5 * 60 * 1000, FailFor = 60 * 1000;

bool Watched(const char* node) {
    if (!node) return false;
    std::string n(node);
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    return n.find("nwmaster") != std::string::npos;          // nwmaster2.beamdog.net
}

DWORD WINAPI Resolve(LPVOID p) {
    std::string* name = (std::string*)p;
    ULONGLONG t0 = GetTickCount64();
    ADDRINFOA hints = {}; hints.ai_family = AF_UNSPEC;
    PADDRINFOA res = nullptr;
    int r = o_getaddrinfo(name->c_str(), nullptr, &hints, &res);      // the real lookup, off the game's thread
    if (res && p_freeaddrinfo) p_freeaddrinfo(res);
    ULONGLONG ms = GetTickCount64() - t0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        Host& h = g_hosts[*name];
        h.busy = false; h.ok = r == 0; h.at = GetTickCount64();
    }
    Log("net: %s looked up in the background: %s after %llu ms", name->c_str(), r == 0 ? "found" : "failed", ms);
    delete name;
    return 0;
}

// true: let the game's call through (a recent lookup succeeded); false: answer "try again" now
bool Ready(const char* node) {
    std::string n(node);
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    std::lock_guard<std::mutex> lk(g_mx);
    Host& h = g_hosts[n];
    ULONGLONG now = GetTickCount64();
    if (h.ok && now - h.at < OkFor) return true;
    if (!h.busy && (h.ok || h.at == 0 || now - h.at >= FailFor)) {
        h.busy = true;
        HANDLE t = CreateThread(nullptr, 0, Resolve, new std::string(n), 0, nullptr);
        if (t) CloseHandle(t); else h.busy = false;
    }
    return false;
}

int WSAAPI hk_getaddrinfo(PCSTR node, PCSTR service, const ADDRINFOA* hints, PADDRINFOA* res) {
    if (Watched(node) && !Ready(node)) {
        if (res) *res = nullptr;
        if (p_WSASetLastError) p_WSASetLastError(WSATRY_AGAIN);
        return EAI_AGAIN;
    }
    return o_getaddrinfo(node, service, hints, res);
}
hostent* WSAAPI hk_gethostbyname(const char* name) {
    if (Watched(name) && !Ready(name)) {
        if (p_WSASetLastError) p_WSASetLastError(WSATRY_AGAIN);
        return nullptr;
    }
    return o_gethostbyname(name);
}
}  // namespace

void NetFix_Install() {
    HMODULE ws = LoadLibraryA("ws2_32.dll");
    if (!ws) { Log("net: no ws2_32"); return; }
    p_freeaddrinfo = (decltype(p_freeaddrinfo))GetProcAddress(ws, "freeaddrinfo");
    p_WSASetLastError = (decltype(p_WSASetLastError))GetProcAddress(ws, "WSASetLastError");
    void* gai = (void*)GetProcAddress(ws, "getaddrinfo");
    void* ghn = (void*)GetProcAddress(ws, "gethostbyname");
    if (gai && MH_CreateHook(gai, (void*)hk_getaddrinfo, (void**)&o_getaddrinfo) == MH_OK) MH_EnableHook(gai);
    if (ghn && MH_CreateHook(ghn, (void*)hk_gethostbyname, (void**)&o_gethostbyname) == MH_OK) MH_EnableHook(ghn);
    Log("net: master server lookups kept off the game's thread (%s, %s)", o_getaddrinfo ? "getaddrinfo" : "-", o_gethostbyname ? "gethostbyname" : "-");
}
