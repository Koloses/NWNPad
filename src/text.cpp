#include "text.h"
#include "game.h"
#include <windows.h>
#include <vector>
#include <cstring>
#include <cstdint>

int Text_CodePage() {
    static int* loc = (int*)GetProcAddress(GetModuleHandleA(nullptr), "?g_DefaultLocale@Encoding@@3W4Locale@1@A");
    extern int Config_CodePage();
    int cfg = Config_CodePage();
    if (cfg > 0) return cfg;
    int l = loc ? *loc : 3;
    switch (l) { case 1: return 1250; case 2: return 1251; default: return 1252; }
}

std::string G2U(const char* s, int len) {
    if (!s) return std::string();
    if (len < 0) len = (int)strlen(s);
    if (len == 0) return std::string();
    int cp = Text_CodePage();
    int wn = MultiByteToWideChar(cp, 0, s, len, nullptr, 0);
    std::vector<wchar_t> w(wn + 1);
    MultiByteToWideChar(cp, 0, s, len, w.data(), wn);
    int un = WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, nullptr, 0, nullptr, nullptr);
    std::string out(un, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), wn, &out[0], un, nullptr, nullptr);
    return out;
}

std::string G2UClean(const char* s, int len) {
    if (!s) return std::string();
    if (len < 0) len = (int)strlen(s);
    std::string raw;
    raw.reserve(len);
    for (int i = 0; i < len; i++) {
        if (s[i] == '<' && i + 1 < len && (s[i + 1] == 'c' || s[i + 1] == '/')) {
            // <cXYZ> has exactly 3 bytes of colour after 'c'; </c> closes
            if (s[i + 1] == 'c' && i + 5 < len && s[i + 5] == '>') { i += 5; continue; }
            if (s[i + 1] == '/' && i + 3 < len && s[i + 2] == 'c' && s[i + 3] == '>') { i += 3; continue; }
        }
        raw.push_back(s[i]);
    }
    return G2U(raw.c_str(), (int)raw.size());
}

std::string ExoStr(const void* e) {
    if (!e) return std::string();
    const char* p = *(const char* const*)e;
    uint32_t n = *(const uint32_t*)((const char*)e + 8);
    if (!p) return std::string();
    if (n == 0xffffffffu || n > 1000000) n = (uint32_t)strlen(p);
    return G2UClean(p, (int)n);
}
