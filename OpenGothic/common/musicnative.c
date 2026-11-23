#include "musicnative.h"

#include "_Internal.h"

#include <stdlib.h>
#include <string.h>

// Record encoding (little-endian, byte-wise, declaration order of _Internal.h / _Dls.h / _Riff.h):
// u8/u16/u32/u64 as declared; C enums as u32; bool as u8 0|1; float/double as raw bits; GUID as 16 raw
// bytes; string = u8 present (NULL vs not) + u32 length + bytes without NUL; arrays = u32 count + elements;
// sample bytes = u32 length + bytes.

// ---------------------------------------------------------------------------------------------------------
// Writer

typedef struct MnWriter {
	uint8_t* data;
	size_t len;
	size_t cap;
	bool failed;
} MnWriter;

static void mn_bytes(MnWriter* w, void const* src, size_t n) {
	if (w->failed || n == 0) {
		return;
	}
	if (w->len + n > w->cap) {
		size_t cap = w->cap == 0 ? 4096 : w->cap;
		while (cap < w->len + n) {
			cap *= 2;
		}
		uint8_t* grown = realloc(w->data, cap);
		if (grown == NULL) {
			w->failed = true;
			return;
		}
		w->data = grown;
		w->cap = cap;
	}
	memcpy(w->data + w->len, src, n);
	w->len += n;
}

static void mn_u8(MnWriter* w, uint8_t v) {
	mn_bytes(w, &v, 1);
}

static void mn_u16(MnWriter* w, uint16_t v) {
	uint8_t b[2] = {(uint8_t) (v & 0xFFU), (uint8_t) (v >> 8U)};
	mn_bytes(w, b, 2);
}

static void mn_u32(MnWriter* w, uint32_t v) {
	uint8_t b[4];
	for (unsigned i = 0; i < 4; ++i) {
		b[i] = (uint8_t) (v >> (8U * i));
	}
	mn_bytes(w, b, 4);
}

static void mn_u64(MnWriter* w, uint64_t v) {
	uint8_t b[8];
	for (unsigned i = 0; i < 8; ++i) {
		b[i] = (uint8_t) (v >> (8U * i));
	}
	mn_bytes(w, b, 8);
}

static void mn_i16(MnWriter* w, int16_t v) {
	uint16_t u;
	memcpy(&u, &v, sizeof u);
	mn_u16(w, u);
}

static void mn_i32(MnWriter* w, int32_t v) {
	uint32_t u;
	memcpy(&u, &v, sizeof u);
	mn_u32(w, u);
}

static void mn_bool(MnWriter* w, bool v) {
	mn_u8(w, v ? 1 : 0);
}

static void mn_double(MnWriter* w, double v) {
	uint64_t u;
	memcpy(&u, &v, sizeof u);
	mn_u64(w, u);
}

static void mn_count(MnWriter* w, size_t n) {
	if (n > UINT32_MAX) {
		w->failed = true;
		return;
	}
	mn_u32(w, (uint32_t) n);
}

static void mn_str(MnWriter* w, char const* s) {
	mn_u8(w, s != NULL ? 1 : 0);
	if (s != NULL) {
		size_t n = strlen(s);
		mn_count(w, n);
		mn_bytes(w, s, n);
	}
}

static void mn_guid(MnWriter* w, DmGuid const* g) {
	mn_bytes(w, g->data, sizeof g->data);
}

static void mn_version(MnWriter* w, DmVersion const* v) {
	mn_u32(w, v->ms);
	mn_u32(w, v->ls);
}

static void mn_unfo(MnWriter* w, DmUnfo const* u) {
	mn_str(w, u->unam);
}

static void mn_info(MnWriter* w, DmInfo const* i) {
	mn_str(w, i->inam);
	mn_str(w, i->icmt);
	mn_str(w, i->icop);
	mn_str(w, i->ieng);
	mn_str(w, i->isbj);
	mn_str(w, i->isft);
	mn_str(w, i->date);
}

static void mn_reference(MnWriter* w, DmReference const* r) {
	mn_guid(w, &r->class_id);
	mn_u32(w, r->valid_data);
	mn_guid(w, &r->guid);
	mn_str(w, r->name);
	mn_str(w, r->file);
	mn_version(w, &r->version);
}

static void mn_timesig(MnWriter* w, DmTimeSignature const* t) {
	mn_u8(w, t->beats_per_measure);
	mn_u8(w, t->beat);
	mn_u16(w, t->grids_per_beat);
}

static void mn_band(MnWriter* w, DmBand const* b) {
	mn_guid(w, &b->guid);
	mn_unfo(w, &b->info);
	mn_count(w, b->instruments_len);
	for (size_t i = 0; i < b->instruments_len; ++i) {
		DmInstrument const* ins = &b->instruments[i];
		mn_u32(w, ins->patch);
		mn_u32(w, ins->assign_patch);
		for (unsigned k = 0; k < 4; ++k) {
			mn_u32(w, ins->note_ranges[k]);
		}
		mn_u32(w, ins->channel);
		mn_u32(w, (uint32_t) ins->options);
		mn_u8(w, ins->pan);
		mn_u8(w, ins->volume);
		mn_i16(w, ins->transpose);
		mn_u32(w, ins->channel_priority);
		mn_reference(w, &ins->reference);
	}
}

static void mn_wavesample(MnWriter* w, DmDlsWaveSample const* s) {
	mn_u16(w, s->unity_note);
	mn_u16(w, s->fine_tune);
	mn_i32(w, s->gain);
	mn_bool(w, s->allow_truncation);
	mn_bool(w, s->allow_compression);
	mn_bool(w, s->looping);
	mn_bool(w, s->loop_with_release);
	mn_u32(w, s->loop_start);
	mn_u32(w, s->loop_length);
}

static void mn_articulators(MnWriter* w, DmDlsArticulator const* a, uint32_t count) {
	mn_u32(w, count);
	for (uint32_t i = 0; i < count; ++i) {
		mn_u8(w, a[i].level);
		mn_u32(w, a[i].connection_count);
		for (uint32_t k = 0; k < a[i].connection_count; ++k) {
			struct DmDlsArticulatorConnection const* c = &a[i].connections[k];
			mn_u32(w, (uint32_t) c->source);
			mn_u32(w, (uint32_t) c->control);
			mn_u32(w, (uint32_t) c->destination);
			mn_u32(w, (uint32_t) c->output_transform);
			mn_u32(w, (uint32_t) c->control_transform);
			mn_u32(w, (uint32_t) c->source_transform);
			mn_i32(w, c->scale);
		}
	}
}

static DmResult mn_finish(MnWriter* w, uint8_t** out, size_t* len) {
	if (w->failed) {
		free(w->data);
		return DmResult_MEMORY_EXHAUSTED;
	}
	*out = w->data;
	*len = w->len;
	return DmResult_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------
// Reader. Strings and sample bytes are first recorded as relocations into the record; once the whole
// object is built, one backing block receives them (samples first, 8-byte aligned, then NUL-terminated
// strings) and the fields are pointed at it. Every array is allocated with its exact final size before its
// elements are filled, so field addresses never move.

typedef struct MnReloc {
	void* field; // char const** or uint8_t const**
	size_t offset;
	size_t length;
	bool sample;
} MnReloc;

typedef struct MnReader {
	uint8_t const* data;
	size_t len;
	size_t at;
	bool bad;
	bool oom;
	MnReloc* relocs;
	size_t reloc_len;
	size_t reloc_cap;
} MnReader;

static size_t mr_left(MnReader const* r) {
	return r->len - r->at;
}

static void mr_bytes(MnReader* r, void* dst, size_t n) {
	if (r->bad || mr_left(r) < n) {
		r->bad = true;
		memset(dst, 0, n);
		return;
	}
	memcpy(dst, r->data + r->at, n);
	r->at += n;
}

static uint8_t mr_u8(MnReader* r) {
	uint8_t v = 0;
	mr_bytes(r, &v, 1);
	return v;
}

static uint16_t mr_u16(MnReader* r) {
	uint8_t b[2];
	mr_bytes(r, b, 2);
	return (uint16_t) (b[0] | (b[1] << 8U));
}

static uint32_t mr_u32(MnReader* r) {
	uint8_t b[4];
	mr_bytes(r, b, 4);
	uint32_t v = 0;
	for (unsigned i = 0; i < 4; ++i) {
		v |= (uint32_t) b[i] << (8U * i);
	}
	return v;
}

static uint64_t mr_u64(MnReader* r) {
	uint8_t b[8];
	mr_bytes(r, b, 8);
	uint64_t v = 0;
	for (unsigned i = 0; i < 8; ++i) {
		v |= (uint64_t) b[i] << (8U * i);
	}
	return v;
}

static int16_t mr_i16(MnReader* r) {
	uint16_t u = mr_u16(r);
	int16_t v;
	memcpy(&v, &u, sizeof v);
	return v;
}

static int32_t mr_i32(MnReader* r) {
	uint32_t u = mr_u32(r);
	int32_t v;
	memcpy(&v, &u, sizeof v);
	return v;
}

static bool mr_bool(MnReader* r) {
	uint8_t v = mr_u8(r);
	if (v > 1) {
		r->bad = true;
	}
	return v == 1;
}

static double mr_double(MnReader* r) {
	uint64_t u = mr_u64(r);
	double v;
	memcpy(&v, &u, sizeof v);
	return v;
}

// An element count whose elements take at least `min_size` bytes each.
static uint32_t mr_count(MnReader* r, size_t min_size) {
	uint32_t n = mr_u32(r);
	if (r->bad || (uint64_t) n * min_size > mr_left(r)) {
		r->bad = true;
		return 0;
	}
	return n;
}

static void mr_reloc(MnReader* r, void* field, size_t length, bool sample) {
	if (r->bad) {
		return;
	}
	if (mr_left(r) < length) {
		r->bad = true;
		return;
	}
	if (r->reloc_len == r->reloc_cap) {
		size_t cap = r->reloc_cap == 0 ? 64 : r->reloc_cap * 2;
		MnReloc* grown = realloc(r->relocs, cap * sizeof *grown);
		if (grown == NULL) {
			r->oom = true;
			r->bad = true;
			return;
		}
		r->relocs = grown;
		r->reloc_cap = cap;
	}
	MnReloc* rel = &r->relocs[r->reloc_len++];
	rel->field = field;
	rel->offset = r->at;
	rel->length = length;
	rel->sample = sample;
	r->at += length;
}

static void mr_str(MnReader* r, char const** field) {
	*field = NULL;
	uint8_t present = mr_u8(r);
	if (present > 1) {
		r->bad = true;
	}
	if (r->bad || present == 0) {
		return;
	}
	uint32_t n = mr_u32(r);
	mr_reloc(r, (void*) field, n, false);
}

static void* mr_alloc(MnReader* r, size_t count, size_t size) {
	if (r->bad || count == 0) {
		return NULL;
	}
	void* mem = Dm_alloc(count * size);
	if (mem == NULL) {
		r->oom = true;
		r->bad = true;
	}
	return mem;
}

// Builds the backing block and points every recorded field at it. Returns NULL with r->oom on failure,
// NULL without failure when nothing needs storage.
static void* mr_backing(MnReader* r) {
	size_t total = 0;
	for (size_t i = 0; i < r->reloc_len; ++i) {
		if (r->relocs[i].sample) {
			total = (total + 7U) & ~(size_t) 7U;
			total += r->relocs[i].length;
		}
	}
	for (size_t i = 0; i < r->reloc_len; ++i) {
		if (!r->relocs[i].sample) {
			total += r->relocs[i].length + 1;
		}
	}
	if (total == 0) {
		return NULL;
	}
	uint8_t* block = Dm_alloc(total);
	if (block == NULL) {
		r->oom = true;
		return NULL;
	}
	size_t at = 0;
	for (size_t i = 0; i < r->reloc_len; ++i) {
		MnReloc const* rel = &r->relocs[i];
		if (!rel->sample) {
			continue;
		}
		at = (at + 7U) & ~(size_t) 7U;
		memcpy(block + at, r->data + rel->offset, rel->length);
		*(uint8_t const**) rel->field = block + at;
		at += rel->length;
	}
	for (size_t i = 0; i < r->reloc_len; ++i) {
		MnReloc const* rel = &r->relocs[i];
		if (rel->sample) {
			continue;
		}
		memcpy(block + at, r->data + rel->offset, rel->length);
		block[at + rel->length] = 0;
		*(char const**) rel->field = (char const*) (block + at);
		at += rel->length + 1;
	}
	return block;
}

static void mr_guid(MnReader* r, DmGuid* g) {
	mr_bytes(r, g->data, sizeof g->data);
}

static void mr_version(MnReader* r, DmVersion* v) {
	v->ms = mr_u32(r);
	v->ls = mr_u32(r);
}

static void mr_unfo(MnReader* r, DmUnfo* u) {
	mr_str(r, &u->unam);
}

static void mr_info(MnReader* r, DmInfo* i) {
	mr_str(r, &i->inam);
	mr_str(r, &i->icmt);
	mr_str(r, &i->icop);
	mr_str(r, &i->ieng);
	mr_str(r, &i->isbj);
	mr_str(r, &i->isft);
	mr_str(r, &i->date);
}

static void mr_reference(MnReader* r, DmReference* ref) {
	mr_guid(r, &ref->class_id);
	ref->valid_data = mr_u32(r);
	mr_guid(r, &ref->guid);
	mr_str(r, &ref->name);
	mr_str(r, &ref->file);
	mr_version(r, &ref->version);
}

static void mr_timesig(MnReader* r, DmTimeSignature* t) {
	t->beats_per_measure = mr_u8(r);
	t->beat = mr_u8(r);
	t->grids_per_beat = mr_u16(r);
}

// Fills a created band; instruments_len counts only filled entries, so a partial band releases cleanly.
static void mr_band(MnReader* r, DmBand* b) {
	mr_guid(r, &b->guid);
	mr_unfo(r, &b->info);
	uint32_t n = mr_count(r, 40);
	b->instruments = mr_alloc(r, n, sizeof(DmInstrument));
	for (uint32_t i = 0; i < n && !r->bad; ++i) {
		DmInstrument* ins = &b->instruments[i];
		b->instruments_len = i + 1;
		ins->patch = mr_u32(r);
		ins->assign_patch = mr_u32(r);
		for (unsigned k = 0; k < 4; ++k) {
			ins->note_ranges[k] = mr_u32(r);
		}
		ins->channel = mr_u32(r);
		ins->options = (DmInstrumentOptions) mr_u32(r);
		ins->pan = mr_u8(r);
		ins->volume = mr_u8(r);
		ins->transpose = mr_i16(r);
		ins->channel_priority = mr_u32(r);
		mr_reference(r, &ins->reference);
	}
}

static void mr_wavesample(MnReader* r, DmDlsWaveSample* s) {
	s->unity_note = mr_u16(r);
	s->fine_tune = mr_u16(r);
	s->gain = mr_i32(r);
	s->allow_truncation = mr_bool(r);
	s->allow_compression = mr_bool(r);
	s->looping = mr_bool(r);
	s->loop_with_release = mr_bool(r);
	s->loop_start = mr_u32(r);
	s->loop_length = mr_u32(r);
}

static void mr_articulators(MnReader* r, DmDlsArticulator** list, uint32_t* count) {
	uint32_t n = mr_count(r, 5);
	*list = mr_alloc(r, n, sizeof(DmDlsArticulator));
	for (uint32_t i = 0; i < n && !r->bad; ++i) {
		DmDlsArticulator* a = &(*list)[i];
		*count = i + 1;
		a->level = mr_u8(r);
		uint32_t c = mr_count(r, 28);
		a->connections = mr_alloc(r, c, sizeof(struct DmDlsArticulatorConnection));
		if (a->connections != NULL) {
			a->connection_count = c;
		}
		for (uint32_t k = 0; k < c && !r->bad; ++k) {
			struct DmDlsArticulatorConnection* con = &a->connections[k];
			con->source = (DmDlsArticulatorSource) mr_u32(r);
			con->control = (DmDlsArticulatorSource) mr_u32(r);
			con->destination = (DmDlsArticulatorDestination) mr_u32(r);
			con->output_transform = (DmDlsArticulatorTransform) mr_u32(r);
			con->control_transform = (DmDlsArticulatorTransform) mr_u32(r);
			con->source_transform = (DmDlsArticulatorTransform) mr_u32(r);
			con->scale = mr_i32(r);
		}
	}
}

static DmResult mr_result(MnReader* r) {
	free(r->relocs);
	r->relocs = NULL;
	if (r->oom) {
		return DmResult_MEMORY_EXHAUSTED;
	}
	return r->bad ? DmResult_FILE_CORRUPT : DmResult_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------
// Old parser wrappers (importer and oracles): exactly what DmLoader_get* does after resolving the bytes.

void MusicNative_releaseSegment(DmSegment* segment) {
	if (segment != NULL) {
		DmSegment_release(segment);
	}
}

void MusicNative_releaseStyle(DmStyle* style) {
	DmStyle_release(style);
}

void MusicNative_releaseDls(DmDls* dls) {
	(void) DmDls_release(dls);
}

// ---------------------------------------------------------------------------------------------------------
// Encoders (importer)

DmResult MusicNative_encodeSegment(DmSegment const* seg, uint8_t** out, size_t* len) {
	*out = NULL;
	*len = 0;
	MnWriter w = {0};
	mn_u32(&w, seg->repeats);
	mn_u32(&w, seg->length);
	mn_u32(&w, seg->play_start);
	mn_u32(&w, seg->loop_start);
	mn_u32(&w, seg->loop_end);
	mn_u32(&w, (uint32_t) seg->resolution);
	mn_guid(&w, &seg->guid);
	mn_unfo(&w, &seg->info);
	mn_version(&w, &seg->version);
	mn_count(&w, seg->messages.length);
	for (size_t i = 0; i < seg->messages.length; ++i) {
		DmMessage const* m = &seg->messages.data[i];
		mn_u32(&w, (uint32_t) m->type);
		mn_u32(&w, m->time);
		switch (m->type) {
		case DmMessage_TEMPO:
			mn_double(&w, m->tempo.tempo);
			break;
		case DmMessage_COMMAND:
			mn_u16(&w, m->command.measure);
			mn_u8(&w, m->command.beat);
			mn_u32(&w, (uint32_t) m->command.command);
			mn_u8(&w, m->command.groove_level);
			mn_u8(&w, m->command.groove_range);
			mn_u32(&w, (uint32_t) m->command.repeat_mode);
			break;
		case DmMessage_CHORD:
			mn_bytes(&w, m->chord.name, sizeof m->chord.name);
			mn_u16(&w, m->chord.measure);
			mn_u8(&w, m->chord.beat);
			mn_bool(&w, m->chord.silent);
			mn_u32(&w, m->chord.subchord_count);
			for (unsigned k = 0; k < 4; ++k) {
				mn_u32(&w, m->chord.subchords[k].chord_pattern);
				mn_u32(&w, m->chord.subchords[k].scale_pattern);
				mn_u32(&w, m->chord.subchords[k].inversion_points);
				mn_u32(&w, m->chord.subchords[k].levels);
				mn_u8(&w, m->chord.subchords[k].chord_root);
				mn_u8(&w, m->chord.subchords[k].scale_root);
			}
			break;
		case DmMessage_BAND:
			mn_u8(&w, m->band.band != NULL ? 1 : 0);
			if (m->band.band != NULL) {
				mn_band(&w, m->band.band);
			}
			break;
		case DmMessage_STYLE:
			mn_reference(&w, &m->style.reference);
			break;
		default:
			free(w.data);
			return DmResult_INVALID_STATE;
		}
	}
	return mn_finish(&w, out, len);
}

DmResult MusicNative_encodeStyle(DmStyle const* sty, uint8_t** out, size_t* len) {
	*out = NULL;
	*len = 0;
	MnWriter w = {0};
	mn_guid(&w, &sty->guid);
	mn_unfo(&w, &sty->info);
	mn_version(&w, &sty->version);
	mn_timesig(&w, &sty->time_signature);
	mn_double(&w, sty->tempo);

	mn_count(&w, sty->bands.length);
	for (size_t i = 0; i < sty->bands.length; ++i) {
		DmBand const* b = sty->bands.data[i];
		mn_u8(&w, b != NULL ? 1 : 0);
		if (b != NULL) {
			mn_band(&w, b);
		}
	}

	mn_count(&w, sty->parts.length);
	for (size_t i = 0; i < sty->parts.length; ++i) {
		DmPart const* p = &sty->parts.data[i];
		mn_unfo(&w, &p->info);
		mn_timesig(&w, &p->time_signature);
		for (unsigned k = 0; k < 32; ++k) {
			mn_u32(&w, p->variation_choices[k]);
		}
		mn_guid(&w, &p->part_id);
		mn_u16(&w, p->length_measures);
		mn_u32(&w, (uint32_t) p->play_mode_flags);
		mn_u8(&w, p->invert_upper);
		mn_u8(&w, p->invert_lower);
		mn_u32(&w, p->note_count);
		for (uint32_t k = 0; k < p->note_count; ++k) {
			DmNote const* n = &p->notes[k];
			mn_u32(&w, n->grid_start);
			mn_u32(&w, n->variation);
			mn_u32(&w, n->duration);
			mn_i16(&w, n->time_offset);
			mn_u16(&w, n->music_value);
			mn_u8(&w, n->velocity);
			mn_u32(&w, n->time_range);
			mn_u32(&w, n->duration_range);
			mn_u8(&w, n->velocity_range);
			mn_u8(&w, n->inversion_id);
			mn_u32(&w, (uint32_t) n->play_mode_flags);
		}
		mn_u32(&w, p->curve_count);
		for (uint32_t k = 0; k < p->curve_count; ++k) {
			DmCurve const* c = &p->curves[k];
			mn_u32(&w, c->grid_start);
			mn_u32(&w, c->variation);
			mn_u32(&w, c->duration);
			mn_u32(&w, c->reset_duration);
			mn_i16(&w, c->time_offset);
			mn_i16(&w, c->start_value);
			mn_i16(&w, c->end_value);
			mn_i16(&w, c->reset_value);
			mn_u32(&w, (uint32_t) c->event_type);
			mn_u32(&w, (uint32_t) c->curve_shape);
			mn_u8(&w, c->cc_data);
			mn_u8(&w, c->flags);
		}
	}

	mn_count(&w, sty->patterns.length);
	for (size_t i = 0; i < sty->patterns.length; ++i) {
		DmPattern const* p = &sty->patterns.data[i];
		mn_unfo(&w, &p->info);
		mn_timesig(&w, &p->time_signature);
		mn_u8(&w, p->groove_bottom);
		mn_u8(&w, p->groove_top);
		mn_u16(&w, p->embellishment);
		mn_u16(&w, p->length_measures);
		mn_u32(&w, p->rhythm_len);
		for (uint32_t k = 0; k < p->rhythm_len; ++k) {
			mn_u32(&w, p->rhythm[k]);
		}
		mn_count(&w, p->parts.length);
		for (size_t k = 0; k < p->parts.length; ++k) {
			DmPartReference const* ref = &p->parts.data[k];
			mn_guid(&w, &ref->part_id);
			mn_unfo(&w, &ref->info);
			mn_u16(&w, ref->logical_part_id);
			mn_u8(&w, ref->variation_lock_id);
			mn_u8(&w, ref->subchord_level);
			mn_u8(&w, ref->priority);
			mn_u32(&w, (uint32_t) ref->random_variation);
		}
	}
	return mn_finish(&w, out, len);
}

DmResult MusicNative_encodeDls(DmDls const* dls, uint8_t** out, size_t* len) {
	*out = NULL;
	*len = 0;
	MnWriter w = {0};
	mn_guid(&w, &dls->guid);
	mn_version(&w, &dls->version);
	mn_info(&w, &dls->info);

	mn_u32(&w, dls->instrument_count);
	for (uint32_t i = 0; i < dls->instrument_count; ++i) {
		DmDlsInstrument const* ins = &dls->instruments[i];
		mn_guid(&w, &ins->guid);
		mn_info(&w, &ins->info);
		mn_u32(&w, ins->bank);
		mn_u32(&w, ins->patch);
		mn_u32(&w, ins->region_count);
		for (uint32_t k = 0; k < ins->region_count; ++k) {
			DmDlsRegion const* rg = &ins->regions[k];
			mn_u16(&w, rg->range_low);
			mn_u16(&w, rg->range_high);
			mn_u16(&w, rg->velocity_low);
			mn_u16(&w, rg->velocity_high);
			mn_u16(&w, rg->key_group);
			mn_bool(&w, rg->nonexclusive);
			mn_info(&w, &rg->info);
			mn_wavesample(&w, &rg->sample);
			mn_bool(&w, rg->link_phase_master);
			mn_bool(&w, rg->link_multi_channel);
			mn_u16(&w, rg->link_phase_group);
			mn_u32(&w, rg->link_channel);
			mn_u32(&w, rg->link_table_index);
			mn_articulators(&w, rg->articulators, rg->articulator_count);
		}
		mn_articulators(&w, ins->articulators, ins->articulator_count);
	}

	mn_u32(&w, dls->pool_table_size);
	for (uint32_t i = 0; i < dls->pool_table_size; ++i) {
		mn_u32(&w, dls->pool_table[i]);
	}

	mn_u32(&w, dls->wave_table_size);
	for (uint32_t i = 0; i < dls->wave_table_size; ++i) {
		DmDlsWave const* wv = &dls->wave_table[i];
		mn_info(&w, &wv->info);
		mn_guid(&w, &wv->guid);
		mn_u32(&w, (uint32_t) wv->format);
		mn_u16(&w, wv->channels);
		mn_u32(&w, wv->samples_per_second);
		mn_u32(&w, wv->avg_bytes_per_second);
		mn_u16(&w, wv->block_align);
		mn_u16(&w, wv->bits_per_sample);
		mn_u16(&w, wv->samples_per_block);
		for (unsigned k = 0; k < 7; ++k) {
			mn_i16(&w, wv->coefficient_table_0[k]);
		}
		for (unsigned k = 0; k < 7; ++k) {
			mn_i16(&w, wv->coefficient_table_1[k]);
		}
		mn_wavesample(&w, &wv->sample);
		mn_u32(&w, wv->pcm_size);
		mn_u8(&w, wv->pcm != NULL ? 1 : 0);
		if (wv->pcm != NULL) {
			mn_bytes(&w, wv->pcm, wv->pcm_size);
		}
	}
	return mn_finish(&w, out, len);
}

// ---------------------------------------------------------------------------------------------------------
// Factories (runtime)

DmResult MusicNative_segment(uint8_t const* bytes, size_t len, DmSegment** out) {
	*out = NULL;
	DmSegment* seg = NULL;
	DmResult rv = DmSegment_create(&seg);
	if (rv != DmResult_SUCCESS) {
		return rv;
	}

	MnReader r = {0};
	r.data = bytes;
	r.len = len;
	seg->repeats = mr_u32(&r);
	seg->length = mr_u32(&r);
	seg->play_start = mr_u32(&r);
	seg->loop_start = mr_u32(&r);
	seg->loop_end = mr_u32(&r);
	seg->resolution = (DmResolveFlags) mr_u32(&r);
	mr_guid(&r, &seg->guid);
	mr_unfo(&r, &seg->info);
	mr_version(&r, &seg->version);

	uint32_t n = mr_count(&r, 8);
	seg->messages.data = mr_alloc(&r, n, sizeof(DmMessage));
	if (seg->messages.data != NULL) {
		seg->messages.capacity = n;
	}
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		DmMessage* m = &seg->messages.data[i];
		uint32_t type = mr_u32(&r);
		m->time = mr_u32(&r);
		if (r.bad) {
			break;
		}
		switch (type) {
		case DmMessage_TEMPO:
			m->type = DmMessage_TEMPO;
			seg->messages.length = i + 1;
			m->tempo.tempo = mr_double(&r);
			break;
		case DmMessage_COMMAND:
			m->type = DmMessage_COMMAND;
			seg->messages.length = i + 1;
			m->command.measure = mr_u16(&r);
			m->command.beat = mr_u8(&r);
			m->command.command = (DmCommandType) mr_u32(&r);
			m->command.groove_level = mr_u8(&r);
			m->command.groove_range = mr_u8(&r);
			m->command.repeat_mode = (DmPatternSelectMode) mr_u32(&r);
			break;
		case DmMessage_CHORD:
			m->type = DmMessage_CHORD;
			seg->messages.length = i + 1;
			mr_bytes(&r, m->chord.name, sizeof m->chord.name);
			m->chord.measure = mr_u16(&r);
			m->chord.beat = mr_u8(&r);
			m->chord.silent = mr_bool(&r);
			m->chord.subchord_count = mr_u32(&r);
			for (unsigned k = 0; k < 4; ++k) {
				m->chord.subchords[k].chord_pattern = mr_u32(&r);
				m->chord.subchords[k].scale_pattern = mr_u32(&r);
				m->chord.subchords[k].inversion_points = mr_u32(&r);
				m->chord.subchords[k].levels = mr_u32(&r);
				m->chord.subchords[k].chord_root = mr_u8(&r);
				m->chord.subchords[k].scale_root = mr_u8(&r);
			}
			break;
		case DmMessage_BAND: {
			m->type = DmMessage_BAND;
			seg->messages.length = i + 1;
			uint8_t present = mr_u8(&r);
			if (present > 1) {
				r.bad = true;
			}
			if (present == 1 && !r.bad) {
				if (DmBand_create(&m->band.band) != DmResult_SUCCESS) {
					r.oom = true;
					r.bad = true;
					break;
				}
				mr_band(&r, m->band.band);
			}
			break;
		}
		case DmMessage_STYLE:
			m->type = DmMessage_STYLE;
			seg->messages.length = i + 1;
			mr_reference(&r, &m->style.reference);
			break;
		default:
			r.bad = true;
			break;
		}
	}
	if (r.at != r.len) {
		r.bad = true;
	}
	if (!r.bad) {
		seg->backing_memory = mr_backing(&r);
	}
	rv = mr_result(&r);
	if (rv != DmResult_SUCCESS) {
		DmSegment_release(seg);
		return rv;
	}
	*out = seg;
	return DmResult_SUCCESS;
}

DmResult MusicNative_style(uint8_t const* bytes, size_t len, DmStyle** out) {
	*out = NULL;
	DmStyle* sty = NULL;
	DmResult rv = DmStyle_create(&sty);
	if (rv != DmResult_SUCCESS) {
		return rv;
	}

	MnReader r = {0};
	r.data = bytes;
	r.len = len;
	mr_guid(&r, &sty->guid);
	mr_unfo(&r, &sty->info);
	mr_version(&r, &sty->version);
	mr_timesig(&r, &sty->time_signature);
	sty->tempo = mr_double(&r);

	uint32_t n = mr_count(&r, 1);
	sty->bands.data = mr_alloc(&r, n, sizeof(DmBand*));
	if (sty->bands.data != NULL) {
		sty->bands.capacity = n;
	}
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		sty->bands.length = i + 1;
		uint8_t present = mr_u8(&r);
		if (present > 1) {
			r.bad = true;
		}
		if (present == 1 && !r.bad) {
			if (DmBand_create(&sty->bands.data[i]) != DmResult_SUCCESS) {
				r.oom = true;
				r.bad = true;
				break;
			}
			mr_band(&r, sty->bands.data[i]);
		}
	}

	n = mr_count(&r, 150);
	sty->parts.data = mr_alloc(&r, n, sizeof(DmPart));
	if (sty->parts.data != NULL) {
		sty->parts.capacity = n;
	}
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		DmPart* p = &sty->parts.data[i];
		DmPart_init(p);
		sty->parts.length = i + 1;
		mr_unfo(&r, &p->info);
		mr_timesig(&r, &p->time_signature);
		for (unsigned k = 0; k < 32; ++k) {
			p->variation_choices[k] = mr_u32(&r);
		}
		mr_guid(&r, &p->part_id);
		p->length_measures = mr_u16(&r);
		p->play_mode_flags = (DmPlayModeFlags) mr_u32(&r);
		p->invert_upper = mr_u8(&r);
		p->invert_lower = mr_u8(&r);

		uint32_t notes = mr_count(&r, 30);
		p->notes = mr_alloc(&r, notes, sizeof(DmNote));
		if (p->notes != NULL) {
			p->note_count = notes;
		}
		for (uint32_t k = 0; k < notes && !r.bad; ++k) {
			DmNote* note = &p->notes[k];
			note->grid_start = mr_u32(&r);
			note->variation = mr_u32(&r);
			note->duration = mr_u32(&r);
			note->time_offset = mr_i16(&r);
			note->music_value = mr_u16(&r);
			note->velocity = mr_u8(&r);
			note->time_range = mr_u32(&r);
			note->duration_range = mr_u32(&r);
			note->velocity_range = mr_u8(&r);
			note->inversion_id = mr_u8(&r);
			note->play_mode_flags = (DmPlayModeFlags) mr_u32(&r);
		}

		uint32_t curves = mr_count(&r, 34);
		p->curves = mr_alloc(&r, curves, sizeof(DmCurve));
		if (p->curves != NULL) {
			p->curve_count = curves;
		}
		for (uint32_t k = 0; k < curves && !r.bad; ++k) {
			DmCurve* c = &p->curves[k];
			c->grid_start = mr_u32(&r);
			c->variation = mr_u32(&r);
			c->duration = mr_u32(&r);
			c->reset_duration = mr_u32(&r);
			c->time_offset = mr_i16(&r);
			c->start_value = mr_i16(&r);
			c->end_value = mr_i16(&r);
			c->reset_value = mr_i16(&r);
			c->event_type = (DmCurveType) mr_u32(&r);
			c->curve_shape = (DmCurveShape) mr_u32(&r);
			c->cc_data = mr_u8(&r);
			c->flags = mr_u8(&r);
		}
	}

	n = mr_count(&r, 16);
	sty->patterns.data = mr_alloc(&r, n, sizeof(DmPattern));
	if (sty->patterns.data != NULL) {
		sty->patterns.capacity = n;
	}
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		DmPattern* p = &sty->patterns.data[i];
		DmPattern_init(p);
		sty->patterns.length = i + 1;
		mr_unfo(&r, &p->info);
		mr_timesig(&r, &p->time_signature);
		p->groove_bottom = mr_u8(&r);
		p->groove_top = mr_u8(&r);
		p->embellishment = mr_u16(&r);
		p->length_measures = mr_u16(&r);

		uint32_t rhythm = mr_count(&r, 4);
		p->rhythm = mr_alloc(&r, rhythm, sizeof(uint32_t));
		if (p->rhythm != NULL) {
			p->rhythm_len = rhythm;
		}
		for (uint32_t k = 0; k < rhythm && !r.bad; ++k) {
			p->rhythm[k] = mr_u32(&r);
		}

		uint32_t refs = mr_count(&r, 26);
		p->parts.data = mr_alloc(&r, refs, sizeof(DmPartReference));
		if (p->parts.data != NULL) {
			p->parts.capacity = refs;
		}
		for (uint32_t k = 0; k < refs && !r.bad; ++k) {
			DmPartReference* ref = &p->parts.data[k];
			DmPartReference_init(ref);
			p->parts.length = k + 1;
			mr_guid(&r, &ref->part_id);
			mr_unfo(&r, &ref->info);
			ref->logical_part_id = mr_u16(&r);
			ref->variation_lock_id = mr_u8(&r);
			ref->subchord_level = mr_u8(&r);
			ref->priority = mr_u8(&r);
			ref->random_variation = (DmVariationType) mr_u32(&r);
		}
	}

	if (r.at != r.len) {
		r.bad = true;
	}
	if (!r.bad) {
		sty->backing_memory = mr_backing(&r);
	}
	rv = mr_result(&r);
	if (rv != DmResult_SUCCESS) {
		DmStyle_release(sty);
		return rv;
	}
	*out = sty;
	return DmResult_SUCCESS;
}

DmResult MusicNative_dls(uint8_t const* bytes, size_t len, DmDls** out) {
	*out = NULL;
	DmDls* dls = NULL;
	DmResult rv = DmDls_create(&dls);
	if (rv != DmResult_SUCCESS) {
		return rv;
	}

	MnReader r = {0};
	r.data = bytes;
	r.len = len;
	mr_guid(&r, &dls->guid);
	mr_version(&r, &dls->version);
	mr_info(&r, &dls->info);

	uint32_t n = mr_count(&r, 35);
	dls->instruments = mr_alloc(&r, n, sizeof(DmDlsInstrument));
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		DmDlsInstrument* ins = &dls->instruments[i];
		dls->instrument_count = i + 1;
		mr_guid(&r, &ins->guid);
		mr_info(&r, &ins->info);
		ins->bank = mr_u32(&r);
		ins->patch = mr_u32(&r);

		uint32_t regions = mr_count(&r, 50);
		ins->regions = mr_alloc(&r, regions, sizeof(DmDlsRegion));
		for (uint32_t k = 0; k < regions && !r.bad; ++k) {
			DmDlsRegion* rg = &ins->regions[k];
			ins->region_count = k + 1;
			rg->range_low = mr_u16(&r);
			rg->range_high = mr_u16(&r);
			rg->velocity_low = mr_u16(&r);
			rg->velocity_high = mr_u16(&r);
			rg->key_group = mr_u16(&r);
			rg->nonexclusive = mr_bool(&r);
			mr_info(&r, &rg->info);
			mr_wavesample(&r, &rg->sample);
			rg->link_phase_master = mr_bool(&r);
			rg->link_multi_channel = mr_bool(&r);
			rg->link_phase_group = mr_u16(&r);
			rg->link_channel = mr_u32(&r);
			rg->link_table_index = mr_u32(&r);
			mr_articulators(&r, &rg->articulators, &rg->articulator_count);
		}
		mr_articulators(&r, &ins->articulators, &ins->articulator_count);
	}

	n = mr_count(&r, 4);
	dls->pool_table = mr_alloc(&r, n, sizeof(uint32_t));
	if (dls->pool_table != NULL) {
		dls->pool_table_size = n;
	}
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		dls->pool_table[i] = mr_u32(&r);
	}

	n = mr_count(&r, 80);
	dls->wave_table = mr_alloc(&r, n, sizeof(DmDlsWave));
	if (dls->wave_table != NULL) {
		dls->wave_table_size = n;
	}
	for (uint32_t i = 0; i < n && !r.bad; ++i) {
		DmDlsWave* wv = &dls->wave_table[i];
		mr_info(&r, &wv->info);
		mr_guid(&r, &wv->guid);
		wv->format = (DmDlsWaveFormat) mr_u32(&r);
		wv->channels = mr_u16(&r);
		wv->samples_per_second = mr_u32(&r);
		wv->avg_bytes_per_second = mr_u32(&r);
		wv->block_align = mr_u16(&r);
		wv->bits_per_sample = mr_u16(&r);
		wv->samples_per_block = mr_u16(&r);
		for (unsigned k = 0; k < 7; ++k) {
			wv->coefficient_table_0[k] = mr_i16(&r);
		}
		for (unsigned k = 0; k < 7; ++k) {
			wv->coefficient_table_1[k] = mr_i16(&r);
		}
		mr_wavesample(&r, &wv->sample);
		wv->pcm_size = mr_u32(&r);
		uint8_t present = mr_u8(&r);
		if (present > 1) {
			r.bad = true;
		}
		if (present == 1) {
			mr_reloc(&r, (void*) &wv->pcm, wv->pcm_size, true);
		}
	}

	if (r.at != r.len) {
		r.bad = true;
	}
	if (!r.bad) {
		dls->backing_memory = mr_backing(&r);
	}
	rv = mr_result(&r);
	if (rv != DmResult_SUCCESS) {
		(void) DmDls_release(dls);
		return rv;
	}
	*out = dls;
	return DmResult_SUCCESS;
}
