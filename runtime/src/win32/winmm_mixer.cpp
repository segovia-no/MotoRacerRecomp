// WINMM mixer API, so the game's Options page can show and change MASTER / WAVE / CD AUDIO
// VOLUME. One mixer device: destination "Speakers" (master) with two sources, "Wave" (sound
// effects) and "CD Audio" (music). Each line has one MIXERCONTROL_CONTROLTYPE_VOLUME control.
#include <cstdio>
#include <string>

#include "host_audio.h"
#include "win32.h"

using namespace rt;

namespace {

constexpr uint32_t MMSYSERR_NOERROR = 0, MMSYSERR_BADDEVICEID = 2, MMSYSERR_INVALPARAM = 11;
constexpr uint32_t MIXERR_INVALLINE = 1024, MIXERR_INVALCONTROL = 1025;
constexpr uint32_t COMPONENT_DST_SPEAKERS = 4, COMPONENT_SRC_COMPACTDISC = 0x1005, COMPONENT_SRC_WAVEOUT = 0x1008;
constexpr uint32_t CONTROLTYPE_VOLUME = 0x50030001;
constexpr uint32_t MIXERLINE_LINEF_ACTIVE = 1, MIXERLINE_LINEF_SOURCE = 0x80000000;
constexpr uint32_t HMIXER = 0x4D0001;

struct Line {
    uint32_t line_id, dest, source, component, control_id;
    const char *short_name, *name;
};
const Line lines[3] = {
    {0xFFFF0000, 0, 0, COMPONENT_DST_SPEAKERS, 1, "Master", "Master Volume"},
    {0x00001000, 0, 0, COMPONENT_SRC_WAVEOUT, 2, "Wave", "Wave Volume"},
    {0x00001001, 0, 1, COMPONENT_SRC_COMPACTDISC, 3, "CD Audio", "CD Audio Volume"},
};
uint32_t volume[4] = {0, 0xFFFF, 0xFFFF, 0xFFFF};   // by control id, 0..65535


void apply(uint32_t control) {
    float g = static_cast<float>(volume[control]) / 65535.f;
    if (control == 1) host::set_master_gain(g);
    if (control == 2) host::set_sfx_gain(g);
    if (control == 3) host::set_music_gain(g);
}

void trace(const char *what, uint32_t a, uint32_t b) {
    static int n = 0;
    if (n++ < 30) log("mixer: %s(0x%x, 0x%x)\n", what, a, b);
}

const Line *by_id(uint32_t id) {
    for (auto &l : lines) if (l.line_id == id) return &l;
    return nullptr;
}

void fill_line(uint32_t p, const Line &l) {
    uint32_t cb = RD32(p);
    memset(M + p + 4, 0, (cb >= 168 ? 168 : cb) - 4);
    WR32(p + 4, l.dest);
    WR32(p + 8, l.source);
    WR32(p + 12, l.line_id);
    WR32(p + 16, MIXERLINE_LINEF_ACTIVE | (l.component >= 0x1000 ? MIXERLINE_LINEF_SOURCE : 0));
    WR32(p + 24, l.component);
    WR32(p + 28, 2);                                       // stereo
    WR32(p + 32, l.component >= 0x1000 ? 0 : 2);           // speakers have 2 connections
    WR32(p + 36, 1);                                       // one control
    write_cstr(p + 40, l.short_name, 16);
    write_cstr(p + 56, l.name, 64);
    WR32(p + 120, l.component == COMPONENT_SRC_COMPACTDISC ? 5 : l.component == COMPONENT_SRC_WAVEOUT ? 1 : 0);
    write_cstr(p + 136, "Moto Racer Recomp Mixer", 32);
}

void fill_control(uint32_t p, const Line &l) {
    memset(M + p, 0, 148);
    WR32(p + 0, 148);
    WR32(p + 4, l.control_id);
    WR32(p + 8, CONTROLTYPE_VOLUME);
    WR32(p + 12, 1);                                       // MIXERCONTROL_CONTROLF_UNIFORM
    write_cstr(p + 20, "Volume", 16);
    write_cstr(p + 36, l.name, 64);
    WR32(p + 100, 0);                                      // minimum
    WR32(p + 104, 0xFFFF);                                 // maximum
    WR32(p + 116, 0xFFFF);                                 // steps
}

}  // namespace

STDCALL(WINMM, mixerGetNumDevs, 0) { return 1; }

STDCALL(WINMM, mixerOpen, 5) {
    trace("mixerOpen", arg(1), arg(4));
    if (arg(1) != 0) return MMSYSERR_BADDEVICEID;
    if (arg(0)) WR32(arg(0), HMIXER);
    return MMSYSERR_NOERROR;
}

STDCALL(WINMM, mixerClose, 1) { return MMSYSERR_NOERROR; }

STDCALL(WINMM, mixerGetLineInfoA, 3) {
    uint32_t p = arg(1), flags = arg(2) & 0xF;
    trace("mixerGetLineInfoA", flags, flags == 3 ? RD32(p + 24) : RD32(p + 12));
    const Line *l = nullptr;
    switch (flags) {
    case 0: l = RD32(p + 4) == 0 ? &lines[0] : nullptr; break;            // DESTINATION
    case 1: l = RD32(p + 8) < 2 ? &lines[1 + RD32(p + 8)] : nullptr; break; // SOURCE
    case 2: l = by_id(RD32(p + 12)); break;                                  // LINEID
    case 3:                                                                  // COMPONENTTYPE
        for (auto &x : lines) if (x.component == RD32(p + 24)) l = &x;
        break;
    default: return MMSYSERR_INVALPARAM;
    }
    if (!l) return MIXERR_INVALLINE;
    fill_line(p, *l);
    return MMSYSERR_NOERROR;
}

STDCALL(WINMM, mixerGetLineControlsA, 3) {
    uint32_t p = arg(1), flags = arg(2) & 0xF;
    trace("mixerGetLineControlsA", flags, RD32(p + 4));
    const Line *l = nullptr;
    if (flags == 1) {   // ONEBYID
        for (auto &x : lines) if (x.control_id == RD32(p + 8)) l = &x;
    } else {
        l = by_id(RD32(p + 4));
        if (l && flags == 2 && RD32(p + 8) != CONTROLTYPE_VOLUME) return MIXERR_INVALCONTROL;   // ONEBYTYPE
    }
    if (!l) return flags == 1 ? MIXERR_INVALCONTROL : MIXERR_INVALLINE;
    WR32(p + 12, 1);
    fill_control(RD32(p + 20), *l);
    return MMSYSERR_NOERROR;
}

STDCALL(WINMM, mixerGetControlDetailsA, 3) {
    uint32_t p = arg(1), id = RD32(p + 4), ch = RD32(p + 8) ? RD32(p + 8) : 1, details = RD32(p + 20);
    if (id < 1 || id > 3) return MIXERR_INVALCONTROL;
    for (uint32_t c = 0; c < ch && c < 2; c++) WR32(details + 4 * c, volume[id]);
    return MMSYSERR_NOERROR;
}

STDCALL(WINMM, mixerSetControlDetails, 3) {
    uint32_t p = arg(1), id = RD32(p + 4), details = RD32(p + 20);
    if (id < 1 || id > 3) return MIXERR_INVALCONTROL;
    uint32_t v = RD32(details) & 0xFFFF;
    trace("mixerSetControlDetails", id, v);
    volume[id] = v;
    apply(id);
    return MMSYSERR_NOERROR;
}
