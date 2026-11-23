// Cheats in the Atmosphere text format, the ones Eden and Yuzu read: <data>/mods/<mod>/cheats/<build id>.txt, where the
// build id is main's (the first 8 bytes of its module ID in hex, 16 characters). In a file, "[name]" starts a cheat
// and "{name}" the master code, followed by its opcodes as 8-digit hex words. The settings menu turns each one on or
// off (mhr.ini: cheat_<FNV-1a of "mod/name"> = 1); this re-reads that whenever mhr.ini changes, so it applies at once.
//
// The game's own ARM code runs here as it would on the Switch (the loader maps main and jumps into it), so a cheat's
// addresses mean the same thing: main + offset is main, a pointer read from the game's memory is a real pointer. What
// can differ is the heap: ours hands out blocks in a different order, so a cheat with a fixed heap offset may point
// somewhere else (pointer chains from main, the usual kind, don't care). A cheat that patches code works too; the
// page is made writable for the write and the instruction cache flushed.
//
// The virtual machine runs every enabled cheat 12 times a second, as on the console: 16 registers that start at 0
// each run, conditionals (memory, keys, registers) with else, loops, arithmetic, saved and static registers. Every
// read goes through process_vm_readv, so a bad pointer just fails the read instead of crashing the game.

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <dirent.h>
#include <pthread.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nnsdk/host.h"
#include "nnsdk/ini.h"
#include "nnsdk/runtime.h"

namespace nnsdk {
void heap_range(uintptr_t* base, size_t* size);  // os_memory.cpp
}

namespace {

struct Cheat {
    std::string mod, name;
    bool master = false;
    std::vector<uint32_t> code;
};

// One cheat file: its master code (if any) and the rest. Static registers live as long as the file stays loaded.
struct CheatFile {
    std::string mod, path;
    std::vector<Cheat> cheats;
    uint64_t statics[256] = {};
};

uint32_t fnv1a(const std::string& s) {
    uint32_t h = 0x811c9dc5u;
    for (unsigned char c : s) h = (h ^ c) * 0x01000193u;
    return h;
}

std::string build_id_hex() {
    char hex[17];
    for (int i = 0; i < 8; i++) snprintf(hex + 2 * i, 3, "%02X", nnsdk::host().main_build_id[i]);
    return std::string(hex, 16);
}

bool is_hex_word(const std::string& t) {
    return t.size() == 8 && std::all_of(t.begin(), t.end(), [](unsigned char c) { return isxdigit(c) != 0; });
}

void parse(const std::string& path, CheatFile& file) {
    FILE* f = fopen(path.c_str(), "r");
    if (!f) return;
    char line[1024];
    Cheat* cur = nullptr;
    while (fgets(line, sizeof line, f)) {
        std::string s = nnsdk::ini::trim(line);
        if (s.empty()) continue;
        if (s[0] == '[' || s[0] == '{') {
            const char close = s[0] == '[' ? ']' : '}';
            const size_t end = s.find(close);
            file.cheats.push_back(Cheat{file.mod, nnsdk::ini::trim(s.substr(1, end == std::string::npos ? std::string::npos : end - 1)),
                                        s[0] == '{', {}});
            cur = &file.cheats.back();
            continue;
        }
        if (!cur) continue;
        size_t at = 0;
        while (at < s.size()) {
            const size_t a = s.find_first_not_of(" \t", at);
            if (a == std::string::npos) break;
            const size_t b = s.find_first_of(" \t", a);
            const std::string tok = s.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (!is_hex_word(tok)) break;  // (the rest of the line is a comment)
            cur->code.push_back(static_cast<uint32_t>(strtoul(tok.c_str(), nullptr, 16)));
            at = b == std::string::npos ? s.size() : b;
        }
    }
    fclose(f);
}

// The cheat files for this build in the enabled mods (folders starting with '#' or '.' are off, as for romfs)
std::vector<CheatFile> load_files() {
    std::vector<CheatFile> out;
    const std::string id = build_id_hex(), dir = nnsdk::ini::data_dir() + "/mods";
    std::vector<std::string> mods;
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d))
            if (e->d_name[0] != '.' && e->d_name[0] != '#') mods.push_back(e->d_name);
        closedir(d);
    }
    std::sort(mods.begin(), mods.end());
    for (const std::string& mod : mods) {
        const std::string cdir = dir + "/" + mod + "/cheats";
        DIR* d = opendir(cdir.c_str());
        if (!d) continue;
        std::vector<std::string> names;
        while (const dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() < 4 || strcasecmp(n.c_str() + n.size() - 4, ".txt") != 0) continue;
            if (n.size() >= 20 && strncasecmp(n.c_str(), id.c_str(), 16) == 0) names.push_back(n);
            else printf("[cheats] %s/cheats/%s: not for this game version (build %s), skipped\n", mod.c_str(), n.c_str(), id.c_str());
        }
        closedir(d);
        std::sort(names.begin(), names.end());
        for (const std::string& n : names) {
            CheatFile file;
            file.mod = mod;
            file.path = cdir + "/" + n;
            parse(file.path, file);
            if (!file.cheats.empty()) out.push_back(std::move(file));
        }
    }
    return out;
}

// ---- memory -------------------------------------------------------------------------------------------------------

bool read_mem(uint64_t addr, void* out, size_t n) {
    iovec local{out, n}, remote{reinterpret_cast<void*>(addr), n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

std::mutex g_text_m;

bool write_mem(uint64_t addr, uint64_t value, size_t n) {
    const NnsdkHost& h = nnsdk::host();
    const uint64_t main = reinterpret_cast<uint64_t>(h.main_base);
    if (addr >= main && addr + n <= main + h.main_text_size) {
        // Code: the page stays executable (other threads may be running it) while it's also writable
        std::lock_guard lock(g_text_m);
        const uint64_t page = static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
        const uint64_t first = addr & ~(page - 1), last = (addr + n + page - 1) & ~(page - 1);
        void* p = reinterpret_cast<void*>(first);
        if (mprotect(p, last - first, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
            static bool logged = false;
            if (!logged) printf("[cheats] can't make main's code writable (%s): code patches won't apply\n", strerror(errno));
            logged = true;
            return false;
        }
        memcpy(reinterpret_cast<void*>(addr), &value, n);
        mprotect(p, last - first, PROT_READ | PROT_EXEC);
        __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr + n));
        return true;
    }
    uintptr_t heap = 0;
    size_t heap_size = 0;
    nnsdk::heap_range(&heap, &heap_size);
    if (addr >= heap && addr + n <= heap + heap_size) {
        // The heap, written like the game writes it: a page that's being watched for the GPU (watch.cpp) takes the
        // fault and notes the change
        memcpy(reinterpret_cast<void*>(addr), &value, n);
        return true;
    }
    iovec local{&value, n}, remote{reinterpret_cast<void*>(addr), n};
    return process_vm_writev(getpid(), &local, 1, &remote, 1, 0) == static_cast<ssize_t>(n);
}

// ---- the virtual machine --------------------------------------------------------------------------------------------

uint64_t mask(uint64_t v, uint32_t width) { return width >= 8 ? v : v & ((1ull << (width * 8)) - 1); }
bool good_width(uint32_t w) { return w == 1 || w == 2 || w == 4 || w == 8; }
uint32_t nib(uint32_t word, int i) { return (word >> (28 - 4 * i)) & 0xF; }  // i-th hex digit from the left

// How many words the instruction at code[i] takes (0: not one we know)
size_t length(const std::vector<uint32_t>& code, size_t i) {
    const uint32_t w = code[i], op = nib(w, 0), width = nib(w, 1);
    const size_t vwords = width == 8 ? 2 : 1;
    switch (op) {
        case 0x0: case 0x1: return 2 + vwords;
        case 0x2: return 1;
        case 0x3: return nib(w, 1) == 0 ? 2 : 1;
        case 0x4: return 3;
        case 0x5: return 2;
        case 0x6: return 3;
        case 0x7: return 2;
        case 0x8: return 1;
        case 0x9: return nib(w, 5) ? 1 + vwords : 1;
        case 0xA: { const uint32_t t = nib(w, 5); return (t == 2 || t == 4 || t == 5) ? 2 : 1; }
        case 0xC:
            switch (nib(w, 1)) {
                case 0x0: {
                    const uint32_t comp = nib(w, 5), cw = nib(w, 2);
                    if (comp == 0 || comp == 2) return 2;
                    if (comp == 4) return 1 + (cw == 8 ? 2 : 1);
                    return 1;
                }
                case 0x1: case 0x2: case 0x3: return 1;
                default: return 0;
            }
        case 0xF:
            if (nib(w, 1) != 0xF) return 0;
            if (nib(w, 2) == 0x0 || nib(w, 2) == 0x1) return 1;          // pause / resume
            if (nib(w, 2) == 0xF) return (nib(w, 5) == 0 || nib(w, 5) == 2) ? 2 : 1;  // debug log
            return 0;
        default: return 0;
    }
}

bool opens_block(uint32_t w) { return nib(w, 0) == 0x1 || nib(w, 0) == 0x8 || (nib(w, 0) == 0xC && nib(w, 1) == 0x0); }

struct Vm {
    uint64_t reg[16] = {}, saved[16] = {};
    size_t loop_top[16] = {};
    uint64_t* statics = nullptr;
    const std::vector<uint32_t>* code = nullptr;
    std::string error;

    uint64_t base(uint32_t mem) {
        const NnsdkHost& h = nnsdk::host();
        if (mem == 0 || mem == 3) return reinterpret_cast<uint64_t>(h.main_base);  // main (and the ASLR region: main)
        if (mem == 1) {
            uintptr_t heap = 0;
            size_t size = 0;
            nnsdk::heap_range(&heap, &size);
            return heap;
        }
        error = "uses the alias region, which this port doesn't have";
        return 0;
    }
    uint64_t imm(size_t at, uint32_t width) {  // a value of 1 or 2 words (8 bytes: high word first)
        const std::vector<uint32_t>& c = *code;
        return width == 8 ? (uint64_t{c[at]} << 32) | c[at + 1] : mask(c[at], width);
    }
    uint64_t load(uint64_t addr, uint32_t width) {
        uint64_t v = 0;
        if (!read_mem(addr, &v, width)) return 0;  // (a pointer that isn't there reads as 0, like the console's)
        return v;
    }
    static bool compare(uint64_t a, uint64_t b, uint32_t cond) {
        switch (cond) {
            case 1: return a > b;
            case 2: return a >= b;
            case 3: return a < b;
            case 4: return a <= b;
            case 5: return a == b;
            case 6: return a != b;
            default: return false;
        }
    }
    static uint64_t math(uint64_t a, uint64_t b, uint32_t type, bool* ok) {
        switch (type) {
            case 0: return a + b;
            case 1: return a - b;
            case 2: return a * b;
            case 3: return b >= 64 ? 0 : a << b;
            case 4: return b >= 64 ? 0 : a >> b;
            case 5: return a & b;
            case 6: return a | b;
            case 7: return ~a;
            case 8: return a ^ b;
            case 9: return a;
            default: *ok = false; return 0;
        }
    }

    // From the instruction after a conditional that failed: past its else (to run that) or its end
    size_t skip_block(size_t i, bool stop_at_else) {
        const std::vector<uint32_t>& c = *code;
        int depth = 0;
        while (i < c.size()) {
            const size_t len = length(c, i);
            if (!len || i + len > c.size()) return c.size();
            const uint32_t w = c[i];
            if (opens_block(w)) {
                depth++;
            } else if (nib(w, 0) == 0x2) {
                const bool is_else = nib(w, 1) == 1;
                if (depth == 0 && (!is_else || stop_at_else)) return i + 1;
                if (!is_else) depth--;
            }
            i += len;
        }
        return i;
    }

    void run(uint64_t buttons) {
        const std::vector<uint32_t>& c = *code;
        size_t i = 0;
        int budget = 200000;  // (a loop that never ends stops here)
        while (i < c.size() && error.empty()) {
            if (--budget < 0) {
                error = "ran too long (a loop that never ends?)";
                return;
            }
            const size_t len = length(c, i);
            if (!len || i + len > c.size()) {
                char b[64];
                snprintf(b, sizeof b, "unknown or cut instruction %08X", c[i]);
                error = b;
                return;
            }
            const uint32_t w = c[i], op = nib(w, 0);
            size_t next = i + len;
            switch (op) {
                case 0x0: {  // [mem + addr + reg] = value
                    const uint32_t width = nib(w, 1), mem = nib(w, 2), r = nib(w, 3);
                    if (!good_width(width)) { error = "bad width"; return; }
                    const uint64_t addr = base(mem) + ((uint64_t{w & 0xFF} << 32) | c[i + 1]) + reg[r];
                    write_mem(addr, imm(i + 2, width), width);
                    break;
                }
                case 0x1: {  // if [mem + addr (+ reg)] <cond> value
                    const uint32_t width = nib(w, 1), mem = nib(w, 2), cond = nib(w, 3);
                    if (!good_width(width)) { error = "bad width"; return; }
                    uint64_t addr = base(mem) + ((uint64_t{w & 0xFF} << 32) | c[i + 1]);
                    if (nib(w, 4)) addr += reg[nib(w, 5)];
                    if (!compare(load(addr, width), imm(i + 2, width), cond)) next = skip_block(next, true);
                    break;
                }
                case 0x2:  // end, or else: reached at the end of a taken branch, so past the else part
                    if (nib(w, 1) == 1) next = skip_block(next, false);
                    break;
                case 0x3: {
                    const uint32_t r = nib(w, 3);
                    if (nib(w, 1) == 0) {
                        reg[r] = c[i + 1];
                        loop_top[r] = next;
                        if (!reg[r]) {  // (no turns: past the loop's end)
                            int depth = 0;
                            size_t k = next;
                            while (k < c.size()) {
                                const size_t l = length(c, k);
                                if (!l) break;
                                if (nib(c[k], 0) == 0x3 && nib(c[k], 3) == r) {
                                    if (nib(c[k], 1) == 0) depth++;
                                    else if (depth-- == 0) { next = k + l; break; }
                                }
                                k += l;
                            }
                        }
                    } else if (--reg[r] != 0) {
                        next = loop_top[r];
                    }
                    break;
                }
                case 0x4: reg[nib(w, 3)] = (uint64_t{c[i + 1]} << 32) | c[i + 2]; break;
                case 0x5: {  // reg = [mem + addr] or [reg + addr]
                    const uint32_t width = nib(w, 1), mem = nib(w, 2), r = nib(w, 3), from = nib(w, 4);
                    if (!good_width(width)) { error = "bad width"; return; }
                    const uint64_t rel = (uint64_t{w & 0xFF} << 32) | c[i + 1];
                    if (from == 0) reg[r] = load(base(mem) + rel, width);
                    else if (from == 1) reg[r] = load(reg[r] + rel, width);
                    else { error = "a load form this port doesn't know yet"; return; }
                    break;
                }
                case 0x6: {  // [reg (+ reg)] = value, then optionally reg += width
                    const uint32_t width = nib(w, 1), r = nib(w, 3);
                    if (!good_width(width)) { error = "bad width"; return; }
                    const uint64_t addr = reg[r] + (nib(w, 5) ? reg[nib(w, 6)] : 0);
                    write_mem(addr, mask((uint64_t{c[i + 1]} << 32) | c[i + 2], width), width);
                    if (nib(w, 4)) reg[r] += width;
                    break;
                }
                case 0x7: {  // reg = reg <op> value
                    const uint32_t width = nib(w, 1), r = nib(w, 3), type = nib(w, 4);
                    bool ok = type <= 4;
                    const uint64_t v = math(reg[r], c[i + 1], type, &ok);
                    if (!ok) { error = "bad arithmetic"; return; }
                    reg[r] = mask(v, width);
                    break;
                }
                case 0x8:  // if all these keys are held
                    if ((buttons & (w & 0x0FFFFFFF)) != (w & 0x0FFFFFFF)) next = skip_block(next, true);
                    break;
                case 0x9: {  // reg = reg <op> (reg or value)
                    const uint32_t width = nib(w, 1), type = nib(w, 2), dst = nib(w, 3), src = nib(w, 4);
                    if (!good_width(width)) { error = "bad width"; return; }
                    const uint64_t b = nib(w, 5) ? imm(i + 1, width) : reg[nib(w, 6)];
                    bool ok = true;
                    const uint64_t v = math(mask(reg[src], width), mask(b, width), type, &ok);
                    if (!ok) { error = "floating-point arithmetic isn't supported yet"; return; }
                    reg[dst] = mask(v, width);
                    break;
                }
                case 0xA: {  // [address] = reg, then optionally address reg += width
                    const uint32_t width = nib(w, 1), src = nib(w, 2), r = nib(w, 3), type = nib(w, 5);
                    if (!good_width(width)) { error = "bad width"; return; }
                    const uint64_t rel = (uint64_t{w & 0xF} << 32) | (len > 1 ? c[i + 1] : 0);
                    uint64_t addr = 0;
                    switch (type) {
                        case 0: addr = reg[r]; break;
                        case 1: addr = reg[r] + reg[nib(w, 6)]; break;
                        case 2: addr = reg[r] + rel; break;
                        case 3: addr = base(nib(w, 6)) + reg[r]; break;
                        case 4: addr = base(nib(w, 6)) + rel; break;
                        case 5: addr = base(nib(w, 6)) + reg[r] + rel; break;
                        default: error = "bad store form"; return;
                    }
                    if (!error.empty()) return;
                    write_mem(addr, mask(reg[src], width), width);
                    if (nib(w, 4)) reg[r] += width;
                    break;
                }
                case 0xC:
                    switch (nib(w, 1)) {
                        case 0x0: {  // if reg <cond> (memory, a register or a value)
                            const uint32_t width = nib(w, 2), cond = nib(w, 3), r = nib(w, 4), comp = nib(w, 5);
                            if (!good_width(width)) { error = "bad width"; return; }
                            const uint64_t rel = (uint64_t{w & 0xF} << 32) | (len > 1 ? c[i + 1] : 0);
                            uint64_t other = 0;
                            switch (comp) {
                                case 0: other = load(base(nib(w, 6)) + rel, width); break;
                                case 1: other = load(base(nib(w, 6)) + reg[nib(w, 7)], width); break;
                                case 2: other = load(reg[nib(w, 6)] + rel, width); break;
                                case 3: other = load(reg[nib(w, 6)] + reg[nib(w, 7)], width); break;
                                case 4: other = imm(i + 1, width); break;
                                case 5: other = reg[nib(w, 6)]; break;
                                default: error = "bad comparison"; return;
                            }
                            if (!error.empty()) return;
                            if (!compare(mask(reg[r], width), mask(other, width), cond)) next = skip_block(next, true);
                            break;
                        }
                        case 0x1: {  // save / restore / clear one register
                            const uint32_t dst = nib(w, 3), src = nib(w, 5), type = nib(w, 6);
                            if (type == 0) reg[dst] = saved[src];
                            else if (type == 1) saved[dst] = reg[src];
                            else if (type == 2) saved[dst] = 0;
                            else if (type == 3) reg[dst] = 0;
                            break;
                        }
                        case 0x2: {  // the same, for the registers in a mask
                            const uint32_t type = nib(w, 2);
                            for (int k = 0; k < 16; k++) {
                                if (!(w & (1u << k))) continue;
                                if (type == 0) reg[k] = saved[k];
                                else if (type == 1) saved[k] = reg[k];
                                else if (type == 2) saved[k] = 0;
                                else if (type == 3) reg[k] = 0;
                            }
                            break;
                        }
                        case 0x3: {  // static registers: 0x00-0x7F read into a register, 0x80-0xFF written from one
                            const uint32_t idx = (w >> 4) & 0xFF, r = w & 0xF;
                            if (idx < 0x80) reg[r] = statics[idx];
                            else statics[idx] = reg[r];
                            break;
                        }
                    }
                    break;
                default: break;  // (pause, resume, debug log: nothing to do here)
            }
            i = next;
        }
    }
};

// ---- the thread -----------------------------------------------------------------------------------------------------

void cheat_thread() {
    pthread_setname_np(pthread_self(), "Cheats");
    std::this_thread::sleep_for(std::chrono::seconds(3));  // (the heap is set up by then)
    std::vector<CheatFile> files;
    std::vector<std::vector<bool>> enabled;  // per file, per cheat
    std::vector<std::vector<std::string>> errors;
    long long stamp = -1;
    auto mods_stamp = [] {
        struct stat st;
        const std::string dir = nnsdk::ini::data_dir() + "/mods";
        return stat(dir.c_str(), &st) == 0 ? static_cast<long long>(st.st_mtim.tv_sec) * 1000000000ll + st.st_mtim.tv_nsec : 0ll;
    };
    long long mods = -1;
    int tick = 0;
    for (;;) {
        // mhr.ini (a cheat turned on or off) or the mods folder changed: read everything again. The files are small.
        if (tick++ % 12 == 0) {
            const long long s = nnsdk::ini::stamp(), m = mods_stamp();
            if (s != stamp || m != mods) {
                stamp = s;
                mods = m;
                files = load_files();
                const nnsdk::ini::Values ini = nnsdk::ini::read();
                enabled.assign(files.size(), {});
                errors.assign(files.size(), {});
                size_t on = 0, total = 0;
                for (size_t f = 0; f < files.size(); f++) {
                    for (const Cheat& c : files[f].cheats) {
                        char key[24];
                        snprintf(key, sizeof key, "cheat_%08x", fnv1a(c.mod + "/" + c.name));
                        const auto it = ini.find(key);
                        const bool e = !c.master && it != ini.end() && nnsdk::ini::truthy(it->second);
                        enabled[f].push_back(e);
                        errors[f].emplace_back();
                        on += e;
                        total += !c.master;
                    }
                }
                if (total) printf("[cheats] %zu of %zu on, from %zu file(s), build %s\n", on, total, files.size(), build_id_hex().c_str());
            }
        }
        const NnsdkInput* in = nnsdk::host().input;
        const uint64_t buttons = in ? in->buttons.load(std::memory_order_relaxed) : 0;
        for (size_t f = 0; f < files.size(); f++) {
            if (std::find(enabled[f].begin(), enabled[f].end(), true) == enabled[f].end()) continue;
            Vm vm;  // (registers start at 0 each run; the master code's carry over to the cheats after it)
            vm.statics = files[f].statics;
            for (size_t k = 0; k < files[f].cheats.size(); k++) {
                const Cheat& c = files[f].cheats[k];
                if ((!c.master && !enabled[f][k]) || !errors[f][k].empty() || c.code.empty()) continue;
                vm.code = &c.code;
                vm.error.clear();
                vm.run(buttons);
                if (!vm.error.empty()) {
                    errors[f][k] = vm.error;
                    printf("[cheats] \"%s\" (%s) stopped: %s\n", c.name.c_str(), c.mod.c_str(), vm.error.c_str());
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::microseconds(1000000 / 12));
    }
}

}  // namespace

namespace nnsdk {
// From nnsdk_attach: the thread only does anything once a cheat is turned on
void start_cheats() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(cheat_thread).detach(); });
}
}  // namespace nnsdk
