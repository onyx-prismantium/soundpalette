// Single translation unit that instantiates the vendored miniaudio + stb_vorbis
// implementations (PLAN.md §4 integration notes). Deliberately compiled without
// -Wall/-Wextra/-Werror (see core/CMakeLists.txt) since this is third-party source,
// not project code; §13.6's warning-clean requirement applies to soundpalette's own code.
//
// Pattern is the one documented in miniaudio.h's own comments for wiring up stb_vorbis:
// header-only stb_vorbis declarations first, then the miniaudio implementation (which
// detects STB_VORBIS_INCLUDE_STB_VORBIS_H and enables its built-in Vorbis backend), then
// the stb_vorbis implementation itself.
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#undef STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"
