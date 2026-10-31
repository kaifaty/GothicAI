// Copyright © 2024. GothicKit Contributors
// SPDX-License-Identifier: MIT-Modern-Variant
#include "_Internal.h"

DmResult DmLoader_create(DmLoader** slf, DmLoaderOptions opt) {
	if (slf == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	DmLoader* new = *slf = Dm_alloc(sizeof *new);

	if (new == NULL) {
		return DmResult_MEMORY_EXHAUSTED;
	}

	new->reference_count = 1;
	new->autodownload = opt& DmLoader_DOWNLOAD;

	if (mtx_init(&new->lock, mtx_recursive) != thrd_success) {
		Dm_free(new);
		return DmResult_MUTEX_ERROR;
	}

	DmResolverList_init(&new->resolvers);
	DmStyleCache_init(&new->style_cache);
	DmDlsCache_init(&new->dls_cache);

	return DmResult_SUCCESS;
}

DmLoader* DmLoader_retain(DmLoader* slf) {
	if (slf == NULL) {
		return NULL;
	}

	(void) atomic_fetch_add(&slf->reference_count, 1);
	return slf;
}

void DmLoader_release(DmLoader* slf) {
	if (slf == NULL) {
		return;
	}

	size_t refs = atomic_fetch_sub(&slf->reference_count, 1) - 1;
	if (refs != 0) {
		return;
	}

	mtx_destroy(&slf->lock);
	DmStyleCache_free(&slf->style_cache);
	DmDlsCache_free(&slf->dls_cache);
	DmResolverList_free(&slf->resolvers);
	Dm_free(slf);
}

DmResult DmLoader_setFactory(DmLoader* slf, DmLoaderFactory const* factory, void* ctx) {
	if (slf == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	if (mtx_lock(&slf->lock) != thrd_success) {
		return DmResult_MUTEX_ERROR;
	}

	if (factory == NULL) {
		memset(&slf->factory, 0, sizeof slf->factory);
		slf->factory_ctx = NULL;
		slf->has_factory = false;
	} else {
		slf->factory = *factory;
		slf->factory_ctx = ctx;
		slf->has_factory = true;
	}

	(void) mtx_unlock(&slf->lock);
	return DmResult_SUCCESS;
}

// Both native and original-source objects enter the same cache/download path. Source callbacks
// are installed by the import-only addResolver implementation, so runtime users never link parsers.
static bool DmLoader_makeSegment(DmLoader* slf, char const* name, DmSegment** out, DmResult* rv) {
	*out = NULL;
	if (mtx_lock(&slf->lock) != thrd_success) {
		return false;
	}
	if (slf->has_factory) {
		*rv = slf->factory.segment != NULL ? slf->factory.segment(slf->factory_ctx, name, out) : DmResult_NOT_FOUND;
		(void) mtx_unlock(&slf->lock);
	} else {
		DmLoaderFactory source = slf->source_factory;
		(void) mtx_unlock(&slf->lock);
		*rv = source.segment != NULL ? source.segment(slf, name, out) : DmResult_NOT_FOUND;
	}

	if (*rv == DmResult_SUCCESS && *out == NULL) {
		*rv = DmResult_FILE_CORRUPT;
	} else if (*rv != DmResult_SUCCESS && *out != NULL) {
		DmSegment_release(*out);
		*out = NULL;
	}
	return true;
}

static bool DmLoader_makeStyle(DmLoader* slf, char const* file, DmStyle** out, DmResult* rv) {
	*out = NULL;
	if (mtx_lock(&slf->lock) != thrd_success) {
		return false;
	}
	if (slf->has_factory) {
		*rv = slf->factory.style != NULL ? slf->factory.style(slf->factory_ctx, file, out) : DmResult_NOT_FOUND;
		(void) mtx_unlock(&slf->lock);
	} else {
		DmLoaderFactory source = slf->source_factory;
		(void) mtx_unlock(&slf->lock);
		*rv = source.style != NULL ? source.style(slf, file, out) : DmResult_NOT_FOUND;
	}

	if (*rv == DmResult_SUCCESS && *out == NULL) {
		*rv = DmResult_FILE_CORRUPT;
	} else if (*rv != DmResult_SUCCESS && *out != NULL) {
		DmStyle_release(*out);
		*out = NULL;
	}
	return true;
}

static bool DmLoader_makeDls(DmLoader* slf, char const* file, DmDls** out, DmResult* rv) {
	*out = NULL;
	if (mtx_lock(&slf->lock) != thrd_success) {
		return false;
	}
	if (slf->has_factory) {
		*rv = slf->factory.dls != NULL ? slf->factory.dls(slf->factory_ctx, file, out) : DmResult_NOT_FOUND;
		(void) mtx_unlock(&slf->lock);
	} else {
		DmLoaderFactory source = slf->source_factory;
		(void) mtx_unlock(&slf->lock);
		*rv = source.dls != NULL ? source.dls(slf, file, out) : DmResult_NOT_FOUND;
	}

	if (*rv == DmResult_SUCCESS && *out == NULL) {
		*rv = DmResult_FILE_CORRUPT;
	} else if (*rv != DmResult_SUCCESS && *out != NULL) {
		DmDls_release(*out);
		*out = NULL;
	}
	return true;
}

DmResult DmLoader_getSegment(DmLoader* slf, char const* name, DmSegment** segment) {
	if (slf == NULL || name == NULL || segment == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	DmResult rv = DmResult_SUCCESS;
	DmSegment* made = NULL;
	if (DmLoader_makeSegment(slf, name, &made, &rv)) {
		if (rv == DmResult_NOT_FOUND) {
			Dm_report(DmLogLevel_DEBUG, "DmLoader: Segment '%s' not found", name);
			return DmResult_NOT_FOUND;
		}
		if (rv != DmResult_SUCCESS) {
			return rv;
		}

		*segment = made;
		Dm_report(DmLogLevel_DEBUG, "DmLoader: Loading segment '%s'", name);
	} else {
		return DmResult_NOT_FOUND;
	}

	if (!slf->autodownload) {
		return DmResult_SUCCESS;
	}

	rv = DmSegment_download(*segment, slf);
	if (rv != DmResult_SUCCESS) {
		Dm_report(DmLogLevel_ERROR, "DmLoader: Automatic download of segment '%s' failed", name);
		DmSegment_release(*segment);
		return rv;
	}

	Dm_report(DmLogLevel_INFO, "DmLoader: Automatic download of segment '%s' succeeded", name);
	return DmResult_SUCCESS;
}

DmResult DmLoader_getDownloadableSound(DmLoader* slf, DmReference const* ref, DmDls** snd) {
	if (slf == NULL || ref == NULL || snd == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	// If no file reference is provided, we're unable to load this.
	if (ref->file == NULL) {
		return DmResult_NOT_FOUND;
	}

	// See if we have the requested item in the cache.
	if (mtx_lock(&slf->lock) != thrd_success) {
		return DmResult_MUTEX_ERROR;
	}

	for (size_t i = 0; i < slf->dls_cache.length; ++i) {
		if (!DmGuid_equals(&ref->guid, &slf->dls_cache.data[i]->guid)) {
			continue;
		}

		*snd = DmDls_retain(slf->dls_cache.data[i]);

		(void) mtx_unlock(&slf->lock);
		return DmResult_SUCCESS;
	}

	(void) mtx_unlock(&slf->lock);

	DmResult rv = DmResult_SUCCESS;
	DmDls* made = NULL;
	if (DmLoader_makeDls(slf, ref->file, &made, &rv)) {
		if (rv == DmResult_NOT_FOUND) {
			Dm_report(DmLogLevel_DEBUG, "DmLoader: DLS collection '%s' not found", ref->name);
			return DmResult_NOT_FOUND;
		}
		if (rv != DmResult_SUCCESS) {
			return rv;
		}

		*snd = made;
		Dm_report(DmLogLevel_DEBUG, "DmLoader: Loading DLS collection '%s'", ref->file);
	} else {
		return DmResult_NOT_FOUND;
	}

	// Add the new item to the cache
	if (mtx_lock(&slf->lock) != thrd_success) {
		return DmResult_MUTEX_ERROR;
	}

	rv = DmDlsCache_add(&slf->dls_cache, DmDls_retain(*snd));

	(void) mtx_unlock(&slf->lock);
	return rv;
}

DmResult DmLoader_getStyle(DmLoader* slf, DmReference const* ref, DmStyle** sty) {
	if (slf == NULL || ref == NULL || sty == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	// If no file reference is provided, we're unable to load this.
	if (ref->file == NULL) {
		return DmResult_NOT_FOUND;
	}

	// See if we have the requested item in the cache.
	if (mtx_lock(&slf->lock) != thrd_success) {
		return DmResult_MUTEX_ERROR;
	}

	for (size_t i = 0; i < slf->style_cache.length; ++i) {
		if (!DmGuid_equals(&ref->guid, &slf->style_cache.data[i]->guid)) {
			continue;
		}

		*sty = DmStyle_retain(slf->style_cache.data[i]);

		(void) mtx_unlock(&slf->lock);
		return DmResult_SUCCESS;
	}

	(void) mtx_unlock(&slf->lock);

	DmResult rv = DmResult_SUCCESS;
	DmStyle* made = NULL;
	if (DmLoader_makeStyle(slf, ref->file, &made, &rv)) {
		if (rv == DmResult_NOT_FOUND) {
			Dm_report(DmLogLevel_DEBUG, "DmLoader: Style '%s' not found", ref->name);
			return DmResult_NOT_FOUND;
		}
		if (rv != DmResult_SUCCESS) {
			return rv;
		}

		*sty = made;
		Dm_report(DmLogLevel_DEBUG, "DmLoader: Loading style '%s'", ref->file);
	} else {
		return DmResult_NOT_FOUND;
	}

	// Add the new item to the cache
	if (mtx_lock(&slf->lock) != thrd_success) {
		return DmResult_MUTEX_ERROR;
	}

	rv = DmStyleCache_add(&slf->style_cache, DmStyle_retain(*sty));

	(void) mtx_unlock(&slf->lock);
	return rv;
}
