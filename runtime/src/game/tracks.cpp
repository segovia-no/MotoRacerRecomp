// All tracks in one list (GitHub issue #9, Recomp menu "ALL TRACKS", on by default). The game's track sets are
// "data disks" (Options > Data disk): disk 0 is Data\list.spt with the eight original tracks, disk 1
// (list01, TWONEWISLANDS) swaps Rock Forest and Lost Ruins for Fun Fair and Sea of Sand. With the
// setting on and disk 0 active, the tracks of disk 1 that disk 0 lacks are appended after each load
// of the active list, so single races offer all ten. The list is a heap array of 0x130-byte records
// at [0x4dc088] ({?, name[0x20] at +4, folder[0x104] at +0x24, ...}), [0x51d298] of them; the game
// walks it by that count, and best times are keyed by the track name. The extra tracks sit at the
// end, so cutting the count back hides them again, also while the game writes the list back to
// LIST.BPT (unlock flags), which must keep the original eight.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "game/settings.h"
#include "runtime.h"
#include "win32.h"

using namespace rt;

namespace {

constexpr uint32_t LIST = 0x4dc088, COUNT = 0x51d298, RECORD = 0x130;
constexpr uint32_t DISK = 0x4dc048;          // the active data disk (0 = the original tracks)
constexpr uint32_t CURRENT = 0x4dc08c;       // the selected track (index into the list)
constexpr uint32_t POCKET = 0x4dc03c, REVERSE = 0x4dc040, LIST_NAME = 0x504eb8;   // also set by the parse
// script reader mode: [0x4e714c] = 1, [0x4e7148] = 0 reads the compiled .BPT files. At exit the game
// writes the list as Data\list.Spt (0x433183), flips them (0x4331bb) so that reading list.Spt compiles
// it into LIST.BPT (0x433218), flips them back (0x433239) and deletes list.Spt (0x433252). A read in
// that mode truncates the .BPT it would compile: nothing is loaded while it is on.
constexpr uint32_t READ_COMPILED = 0x4e714c, READ_SOURCE = 0x4e7148;
constexpr uint32_t LIST_CTOR = 0x428c70, LIST_DTOR = 0x428d10, OPEN_READ = 0x4246a0, CLOSE = 0x4246e0,
                   PARSE = 0x428d60, MALLOC = 0x4c5381, FREE = 0x4c53c1;

struct Merged {
    bool on = false;
    uint32_t list = 0, base = 0, total = 0;
} merged;

std::string folder(uint32_t record) {
    std::string s;
    for (uint32_t k = 0; k < 0x104 && RD8(record + 0x24 + k); k++) s += static_cast<char>(tolower(RD8(record + 0x24 + k)));
    return s;
}

std::string name(uint32_t record) {
    std::string s;
    for (uint32_t k = 0; k < 0x20 && RD8(record + 4 + k); k++) s += static_cast<char>(RD8(record + 4 + k));
    return s;
}

bool active() { return merged.on && RD32(LIST) == merged.list; }

// Loads data disk 1's list through the game's own reader; the parse replaces the list globals.
bool load_disk1() {
    static uint32_t obj = 0, path = 0;
    if (!obj) {
        obj = heap_alloc(0x200, true);
        path = heap_alloc(32, true);
        const char *p = "Data\\list01.spt";
        for (uint32_t i = 0; i <= strlen(p); i++) WR8(path + i, static_cast<uint8_t>(p[i]));
    }
    const uint32_t compiled = RD32(READ_COMPILED), source = RD32(READ_SOURCE);
    WR32(READ_COMPILED, 1);
    WR32(READ_SOURCE, 0);
    guest_call_thiscall(LIST_CTOR, obj, {});
    guest_call_thiscall(OPEN_READ, obj, {path, 1});
    const uint32_t ok = guest_call_thiscall(PARSE, obj, {0, 0, 0});
    guest_call_thiscall(CLOSE, obj, {});
    guest_call_thiscall(LIST_DTOR, obj, {});
    WR32(READ_COMPILED, compiled);
    WR32(READ_SOURCE, source);
    return ok != 0;
}

void merge() {
    if (!settings::get().all_tracks || RD32(DISK) != 0 || active() || RD32(READ_SOURCE)) return;
    const uint32_t n0 = RD32(COUNT), p0 = RD32(LIST);
    if (!p0 || n0 == 0 || n0 > 64) return;
    if (FILE *f = fopen(w32::host_path("Data\\LIST01.BPT").c_str(), "rb")) {
        fclose(f);
    } else {
        log("all tracks: no data disk list (Data\\LIST01.BPT)\n");
        return;
    }
    // the original list and the parse's other globals, restored after loading disk 1
    std::vector<uint8_t> base(n0 * RECORD), list_name(0x20);
    for (uint32_t i = 0; i < base.size(); i++) base[i] = RD8(p0 + i);
    for (uint32_t i = 0; i < list_name.size(); i++) list_name[i] = RD8(LIST_NAME + i);
    const uint32_t pocket = RD32(POCKET), reverse = RD32(REVERSE), current = RD32(CURRENT);
    const uint32_t saved[7] = {R.eax, R.ecx, R.edx, R.ebx, R.esi, R.edi, R.ebp};
    const bool ok = load_disk1();
    R.eax = saved[0], R.ecx = saved[1], R.edx = saved[2], R.ebx = saved[3], R.esi = saved[4], R.edi = saved[5], R.ebp = saved[6];
    const uint32_t n1 = ok ? RD32(COUNT) : 0, p1 = ok ? RD32(LIST) : 0;
    std::vector<uint32_t> extra;   // disk 1 records whose folder the original list lacks
    if (p1 && n1 <= 64)
        for (uint32_t i = 0; i < n1; i++) {
            const std::string f = folder(p1 + i * RECORD);
            bool known = false;
            for (uint32_t k = 0; k < n0 && !known; k++) {
                std::string g;
                for (uint32_t c = 0; c < 0x104 && base[k * RECORD + 0x24 + c]; c++) g += static_cast<char>(tolower(base[k * RECORD + 0x24 + c]));
                known = g == f;
            }
            if (!known) extra.push_back(p1 + i * RECORD);
        }
    const uint32_t total = n0 + static_cast<uint32_t>(extra.size());
    const uint32_t buf = guest_call_cdecl(MALLOC, {total * RECORD});
    for (uint32_t i = 0; i < base.size(); i++) WR8(buf + i, base[i]);
    for (size_t e = 0; e < extra.size(); e++)
        for (uint32_t i = 0; i < RECORD; i++) WR8(buf + (n0 + static_cast<uint32_t>(e)) * RECORD + i, RD8(extra[e] + i));
    std::string added;
    for (uint32_t e : extra) added += " " + name(e);
    if (p1 && p1 != buf) guest_call_cdecl(FREE, {p1});
    WR32(LIST, buf);
    WR32(COUNT, total);
    WR32(POCKET, pocket);
    WR32(REVERSE, reverse);
    WR32(CURRENT, current);
    for (uint32_t i = 0; i < list_name.size(); i++) WR8(LIST_NAME + i, list_name[i]);
    R.eax = saved[0], R.ecx = saved[1], R.edx = saved[2], R.ebx = saved[3], R.esi = saved[4], R.edi = saved[5], R.ebp = saved[6];
    merged = {true, buf, n0, total};
    log("all tracks: %u + %zu from data disk 1:%s%s\n", n0, extra.size(), added.c_str(), ok ? "" : " (list01 not read)");
}

}  // namespace

namespace game {

// The Recomp menu toggled the setting: merge now, or hide the extra tracks again.
void tracks_apply() {
    if (settings::get().all_tracks) {
        merge();
    } else if (active()) {
        WR32(COUNT, merged.base);
        if (RD32(CURRENT) >= merged.base) WR32(CURRENT, 0);
        merged.on = false;
        log("all tracks: off (%u tracks)\n", merged.base);
    }
}

}  // namespace game

// GAME_HOOK after the active list was read at startup (0x472d73). A selection past the end (a merged
// list at the last run) goes back to the first track. (The other read of the active list, 0x433218,
// is the exit's recompile of list.Spt: a merge there truncated LIST01.BPT and looped on the missing
// list01.spt until the guest heap ran out.)
extern "C" void game_hook_tracks_loaded(void) {
    merge();
    if (RD32(CURRENT) >= RD32(COUNT)) WR32(CURRENT, 0);
}

// GAME_HOOKs around the list write-back 0x4291f0 (0x433183 / 0x433188): the original tracks only.
extern "C" void game_hook_tracks_save_begin(void) {
    if (active()) WR32(COUNT, merged.base);
}

extern "C" void game_hook_tracks_save_end(void) {
    if (active()) WR32(COUNT, merged.total);
}
