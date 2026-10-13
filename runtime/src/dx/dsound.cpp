// DirectSound: not implemented yet; the game runs silently (it supports -NoSound).
#include "com.h"

using namespace rt;

STDCALL(DSOUND, DirectSoundCreate, 3) {
    log("DirectSoundCreate -> DSERR_NODRIVER (audio not implemented yet)\n");
    if (arg(1)) WR32(arg(1), 0);
    return 0x88780078;   // DSERR_NODRIVER
}
