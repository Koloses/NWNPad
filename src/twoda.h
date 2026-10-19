// NWNPad - text 2DA tables read through the game's resource manager
#pragma once
#include <string>
#include <vector>
#include <map>

class TwoDA {
public:
    bool Load(const char* resref);                 // true if found
    bool ok() const { return !cols_.empty(); }
    int Rows() const { return (int)rows_.size(); }
    int Col(const char* name) const;               // -1 if missing (case-insensitive)
    // "" for **** or out of range
    const std::string& Get(int row, int col) const;
    const std::string& Get(int row, const char* col) const { return Get(row, Col(col)); }
    int Int(int row, int col, int def = -1) const;
    int Int(int row, const char* col, int def = -1) const { return Int(row, Col(col), def); }
private:
    std::vector<std::string> cols_;
    std::vector<std::vector<std::string>> rows_;
};

// cached tables
const TwoDA& TDA(const char* resref);
