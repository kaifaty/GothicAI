#pragma once

// P10 import evidence for the GothicKit DirectMusic provider. The trace runs the
// unchanged dmusic loader (parse, then the same download the game's DmLoader_DOWNLOAD
// performs) with the caller's resolver, then reads the references from the parsed
// structures. No performance, synthesizer or PCM is created.
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void* BaseMusicResolve(void* ctx, char const* file, size_t* len);

typedef struct BaseMusicReference {
  int         style;        // 1: style of a segment message, 0: band instrument
  size_t      message;      // index of the segment message
  size_t      band;         // style band index, or SIZE_MAX for a segment band
  size_t      instrument;
  char const* file;         // as the loader requests it, after gm/gs/xg substitution; may be NULL
  char const* name;
  uint8_t     guid[16];     // the reference GUID the loader's cache compares
  uint32_t    options;      // instrument flags
  int         downloadable; // instrument has a valid patch or bank select
  void const* object;       // loaded style or DLS shared through the loader's GUID cache, or NULL
  uint8_t     object_guid[16];
  } BaseMusicReference;

typedef void BaseMusicVisit(void* ctx, BaseMusicReference const* reference);

// Returns the DmResult that DmLoader_getSegment with DmLoader_DOWNLOAD returns for `root`.
// *loaded is 1 when the segment parsed and its references were visited.
int BaseMusic_trace(char const* root, BaseMusicResolve* resolve, void* resolveCtx,
                    BaseMusicVisit* visit, void* visitCtx, int* loaded);

#ifdef __cplusplus
}
#endif
