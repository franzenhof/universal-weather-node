#pragma once
#include <string>
#include <vector>
// namespaces cleared (cmdFactoryReset() test)
inline std::vector<std::string>& hostPrefsCleared() { static std::vector<std::string> v; return v; }
struct Preferences {
    std::string ns;
    bool begin(const char* n,bool=false,const char* = nullptr) { ns = n ? n : ""; return true; }
    void clear() { hostPrefsCleared().push_back(ns); }
    void end() {}
};
