/* Single translation unit that pulls in the implementations of the
 * header-only dr_libs decoders. Kept separate so their implementations
 * are compiled exactly once. */

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"
