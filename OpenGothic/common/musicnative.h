#pragma once

// P16: GothicKit's parsed segment/style/DLS objects as own record bytes (common/musicdata.h describes the
// envelope) and back. The importer encodes what DmSegment_parse/DmStyle_parse/DmDls_parse produced; the
// runtime factory rebuilds an equivalent fresh object without any RIFF parsing. Only parsed fields are
// stored: no reference counts, locks, capacities, resolved style/DLS/band pointers or synth state.

#include <dmusic.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Importer, before the old parser converts strings in place: every UTF-16 string chunk of the kinds that
// parser converts (LIST UNFO / UNAM, LIST DMRF / name and file, chord crdb names) must lie inside its
// parent chunk, contain a terminating zero unit (within the first 16 units for chord names) and have only
// units <= 0x7F before it. Lists that do not fit their parent are rejected too (other chunks that overrun
// end their level, as the parser's reader does). Returns DmResult_SUCCESS, or DmResult_FILE_CORRUPT with
// *badOffset = byte offset of the offending chunk.
DmResult MusicNative_checkGkStrings(uint8_t const* bytes, size_t len, size_t* badOffset);

// Importer: the old parser exactly as the loader runs it (DmX_create, then DmX_parse adopting a Dm_alloc
// copy of `bytes`). The result is the DmResult the loader would get; *out is NULL on failure.
DmResult MusicNative_parseSegment(uint8_t const* bytes, size_t len, DmSegment** out);
DmResult MusicNative_parseStyle(uint8_t const* bytes, size_t len, DmStyle** out);
DmResult MusicNative_parseDls(uint8_t const* bytes, size_t len, DmDls** out);

// Importer: the record bytes of a parsed object. *out is malloc-ed (release with free()). Returns
// DmResult_INVALID_STATE for a segment message type that cannot come from a file.
DmResult MusicNative_encodeSegment(DmSegment const* segment, uint8_t** out, size_t* len);
DmResult MusicNative_encodeStyle(DmStyle const* style, uint8_t** out, size_t* len);
DmResult MusicNative_encodeDls(DmDls const* dls, uint8_t** out, size_t* len);

// Runtime factory: a fresh object (reference count 1) built from record bytes the way the parser builds
// one. All its strings and DLS sample bytes live in one Dm_alloc block owned as backing_memory (sample
// slices first, 8-byte aligned); arrays are allocated separately, as the existing release functions free
// them. Nothing refers to `bytes` after return. On a malformed record everything built is released once,
// *out is NULL and the result is DmResult_FILE_CORRUPT (or DmResult_MEMORY_EXHAUSTED).
DmResult MusicNative_segment(uint8_t const* bytes, size_t len, DmSegment** out);
DmResult MusicNative_style(uint8_t const* bytes, size_t len, DmStyle** out);
DmResult MusicNative_dls(uint8_t const* bytes, size_t len, DmDls** out);

// Releases objects created by the factories above or by the old parser (for C++ callers that cannot
// see the internal release functions).
void MusicNative_releaseSegment(DmSegment* segment);
void MusicNative_releaseStyle(DmStyle* style);
void MusicNative_releaseDls(DmDls* dls);

#ifdef __cplusplus
}
#endif
