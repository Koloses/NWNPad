// NWNPad - text 2DA tables read through the game's resource manager
#include "twoda.h"
#include "res.h"
#include "log.h"
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace {
const std::string EMPTY;
std::vector<std::string> Split(const std::string& line) {
    std::vector<std::string> out;
    size_t i = 0, n = line.size();
    while (i < n) {
        while (i < n && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
        if (i >= n) break;
        if (line[i] == '"') {
            size_t j = line.find('"', i + 1);
            if (j == std::string::npos) j = n;
            out.push_back(line.substr(i + 1, j - i - 1));
            i = j + 1;
        } else {
            size_t j = i;
            while (j < n && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') j++;
            out.push_back(line.substr(i, j - i));
            i = j;
        }
    }
    return out;
}
}

bool TwoDA::Load(const char* resref) {
    cols_.clear(); rows_.clear();
    std::vector<uint8_t> d;
    if (!Res_Load(resref, 2017, d)) { Log("2da: %s not found", resref); return false; }
    std::string s(d.begin(), d.end());
    size_t pos = 0; int ln = 0;
    while (pos < s.size()) {
        size_t e = s.find('\n', pos);
        if (e == std::string::npos) e = s.size();
        std::string line = s.substr(pos, e - pos);
        pos = e + 1;
        ln++;
        if (ln == 1) continue;                         // "2DA V2.0"
        std::vector<std::string> f = Split(line);
        if (f.empty()) continue;
        if (f[0] == "DEFAULT:") continue;
        if (cols_.empty()) { cols_ = f; continue; }
        int idx = atoi(f[0].c_str());
        if (idx < 0 || idx > 100000) continue;
        if ((int)rows_.size() <= idx) rows_.resize(idx + 1);
        f.erase(f.begin());
        for (auto& v : f) if (v == "****") v.clear();
        rows_[idx] = f;
    }
    return ok();
}

int TwoDA::Col(const char* name) const {
    for (size_t i = 0; i < cols_.size(); i++)
        if (_stricmp(cols_[i].c_str(), name) == 0) return (int)i;
    return -1;
}

const std::string& TwoDA::Get(int row, int col) const {
    if (row < 0 || row >= (int)rows_.size() || col < 0) return EMPTY;
    const auto& r = rows_[row];
    return col < (int)r.size() ? r[col] : EMPTY;
}

int TwoDA::Int(int row, int col, int def) const {
    const std::string& v = Get(row, col);
    if (v.empty()) return def;
    return (int)strtol(v.c_str(), nullptr, 0);
}

const TwoDA& TDA(const char* resref) {
    static std::map<std::string, TwoDA> cache;
    auto it = cache.find(resref);
    if (it != cache.end()) return it->second;
    TwoDA& t = cache[resref];
    t.Load(resref);
    Log("2da: %s %d rows", resref, t.Rows());
    return t;
}
