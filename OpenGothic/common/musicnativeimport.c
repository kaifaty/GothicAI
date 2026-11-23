#include "musicnative.h"
#include "_Internal.h"
#include <string.h>

static void* mn_copy(uint8_t const* bytes, size_t len) {
	void* buf = Dm_alloc(len == 0 ? 1 : len);
	if (buf != NULL && len != 0) {
		memcpy(buf, bytes, len);
	}
	return buf;
}

DmResult MusicNative_parseSegment(uint8_t const* bytes, size_t len, DmSegment** out) {
	*out = NULL;
	void* buf = mn_copy(bytes, len);
	if (buf == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}
	DmSegment* seg = NULL;
	DmResult rv = DmSegment_create(&seg);
	if (rv != DmResult_SUCCESS) {
		Dm_free(buf);
		return rv;
	}
	rv = DmSegment_parse(seg, buf, len);
	if (rv != DmResult_SUCCESS) {
		DmSegment_release(seg);
		return rv;
	}
	*out = seg;
	return DmResult_SUCCESS;
}

DmResult MusicNative_parseStyle(uint8_t const* bytes, size_t len, DmStyle** out) {
	*out = NULL;
	void* buf = mn_copy(bytes, len);
	if (buf == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}
	DmStyle* sty = NULL;
	DmResult rv = DmStyle_create(&sty);
	if (rv != DmResult_SUCCESS) {
		Dm_free(buf);
		return rv;
	}
	rv = DmStyle_parse(sty, buf, len);
	if (rv != DmResult_SUCCESS) {
		DmStyle_release(sty);
		return rv;
	}
	*out = sty;
	return DmResult_SUCCESS;
}

DmResult MusicNative_parseDls(uint8_t const* bytes, size_t len, DmDls** out) {
	*out = NULL;
	void* buf = mn_copy(bytes, len);
	if (buf == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}
	DmDls* dls = NULL;
	DmResult rv = DmDls_create(&dls);
	if (rv != DmResult_SUCCESS) {
		Dm_free(buf);
		return rv;
	}
	rv = DmDls_parse(dls, buf, len);
	if (rv != DmResult_SUCCESS) {
		DmDls_release(dls);
		return rv;
	}
	*out = dls;
	return DmResult_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------
// UTF-16 preflight (importer)

static uint32_t mn_rd32(uint8_t const* p) {
	return (uint32_t) p[0] | ((uint32_t) p[1] << 8U) | ((uint32_t) p[2] << 16U) | ((uint32_t) p[3] << 24U);
}

// A zero unit within the first `max_units` units of the chunk, and only ASCII units before it.
static bool mn_utf16_ok(uint8_t const* mem, size_t len, size_t max_units) {
	size_t units = len / 2;
	if (units > max_units) {
		units = max_units;
	}
	for (size_t i = 0; i < units; ++i) {
		uint16_t u = (uint16_t) (mem[2 * i] | (mem[2 * i + 1] << 8U));
		if (u == 0) {
			return true;
		}
		if (u > 0x7F) {
			return false;
		}
	}
	return false;
}

static bool mn_walk(uint8_t const* base, uint8_t const* mem, size_t len, uint32_t list_type, size_t* bad) {
	size_t pos = 0;
	while (len - pos >= 8) {
		uint8_t const* head = mem + pos;
		uint32_t id = mn_rd32(head);
		size_t body_len = mn_rd32(head + 4);
		size_t header = 8;
		uint32_t type = 0;
		if (id == DM_FOURCC_RIFF || id == DM_FOURCC_LIST) {
			if (body_len < 4 || len - pos < 12) {
				*bad = (size_t) (head - base);
				return false;
			}
			type = mn_rd32(head + 8);
			header = 12;
			body_len -= 4;
		}
		bool const strings = (list_type == DM_FOURCC_UNFO && id == DM_FOURCC_UNAM) ||
		                     (list_type == DM_FOURCC_DMRF && (id == DM_FOURCC_NAME || id == DM_FOURCC_FILE)) ||
		                     id == DM_FOURCC_CRDB;
		if (body_len > len - pos - header) {
			// DmRiff_readChunk does not bound a chunk by its parent. A plain chunk that overruns only ends
			// this level (the reader's position passes its end); a list or string chunk would be read past it.
			if (id != DM_FOURCC_RIFF && id != DM_FOURCC_LIST && !strings) {
				break;
			}
			*bad = (size_t) (head - base);
			return false;
		}
		uint8_t const* body = head + header;
		if (id == DM_FOURCC_RIFF || id == DM_FOURCC_LIST) {
			if (!mn_walk(base, body, body_len, type, bad)) {
				return false;
			}
		} else if (strings && id != DM_FOURCC_CRDB) {
			if (!mn_utf16_ok(body, body_len, SIZE_MAX)) {
				*bad = (size_t) (head - base);
				return false;
			}
		} else if (id == DM_FOURCC_CRDB) {
			// u32 item size, then DMUS_IO_CHORD starting with its 16-unit name; a shorter body reads as zeros.
			if (body_len >= 4 + 32 && !mn_utf16_ok(body + 4, 32, 16)) {
				*bad = (size_t) (head - base);
				return false;
			}
		}
		size_t step = header + body_len + (body_len % 2);
		if (step > len - pos) {
			break;
		}
		pos += step;
	}
	return true;
}

DmResult MusicNative_checkGkStrings(uint8_t const* bytes, size_t len, size_t* badOffset) {
	*badOffset = 0;
	// DmRiff_init: any 12-byte header whose length fits the buffer; its body is walked whatever the id.
	// A buffer it refuses is never parsed, so it has nothing to convert.
	if (len < 12 || mn_rd32(bytes + 4) > len - 8) {
		return DmResult_SUCCESS;
	}
	size_t body_len = mn_rd32(bytes + 4);
	if (body_len < 4) {
		return DmResult_FILE_CORRUPT;
	}
	return mn_walk(bytes, bytes + 12, body_len - 4, mn_rd32(bytes + 8), badOffset) ? DmResult_SUCCESS
	                                                                                : DmResult_FILE_CORRUPT;
}
