#pragma once
// <datos>/mhr.ini: the settings menu's choices, one "name = value" per line ('#' or ';' starts a comment, [sections]
// are only for reading it). A switch is on when its value is 1 / on / true / yes; for the ones without a value in the
// file, the old way still works: an empty file with the switch's name in the data folder (handy with adb).
// Header only: the loader and libnnsdk both use it.

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

namespace nnsdk::ini {

using Values = std::unordered_map<std::string, std::string>;

inline std::string data_dir() {
    const char* data = getenv("NIER_DATA");
    return data ? data : ".";
}

inline std::string path() { return data_dir() + "/mhr.ini"; }

inline std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

inline Values read() {
    Values out;
    FILE* f = fopen(path().c_str(), "r");
    if (!f) return out;
    char buf[1024];
    while (fgets(buf, sizeof buf, f)) {
        const std::string line = trim(buf);
        if (line.empty() || line[0] == '#' || line[0] == ';' || line[0] == '[') continue;
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    fclose(f);
    return out;
}

// When mhr.ini last changed (0: not there), to read it again only then.
inline long long stamp() {
    struct stat st;
    if (stat(path().c_str(), &st) != 0) return 0;
    return static_cast<long long>(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec;
}

// 1 / on / true / yes, or any other number but 0.
inline bool truthy(const std::string& v) {
    if (v == "on" || v == "true" || v == "yes") return true;
    char* end = nullptr;
    const float n = strtof(v.c_str(), &end);
    return end != v.c_str() && n != 0.0f;
}

// The switch's value in the file if it has one, else whether the old empty file is there.
inline bool on(const Values& values, const std::string& name, bool file_there) {
    const auto it = values.find(name);
    return it != values.end() ? truthy(it->second) : file_there;
}

inline bool on(const char* name) {
    return on(read(), name, access((data_dir() + "/" + name).c_str(), F_OK) == 0);
}

// A number from the file (0 when it isn't there or isn't a number).
inline float number(const char* name) {
    const Values values = read();
    const auto it = values.find(name);
    return it != values.end() ? strtof(it->second.c_str(), nullptr) : 0.0f;
}

// Sets name = value in the file, keeping its other lines (an empty value removes the line), and drops the old-style
// empty file of that name so it can't disagree.
inline void set(const char* name, const std::string& value) {
    std::vector<std::string> lines;
    bool done = false;
    if (FILE* f = fopen(path().c_str(), "r")) {
        char buf[1024];
        while (fgets(buf, sizeof buf, f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            const std::string t = trim(line);
            const size_t eq = t.find('=');
            if (!t.empty() && t[0] != '#' && t[0] != ';' && t[0] != '[' && eq != std::string::npos &&
                trim(t.substr(0, eq)) == name) {
                if (!done && !value.empty()) lines.push_back(std::string(name) + " = " + value);
                done = true;
                continue;
            }
            lines.push_back(line);
        }
        fclose(f);
    }
    if (!done && !value.empty()) lines.push_back(std::string(name) + " = " + value);
    const std::string tmp = path() + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "w")) {
        for (const std::string& l : lines) fprintf(f, "%s\n", l.c_str());
        fclose(f);
        rename(tmp.c_str(), path().c_str());
    }
    unlink((data_dir() + "/" + name).c_str());
}

}  // namespace nnsdk::ini
