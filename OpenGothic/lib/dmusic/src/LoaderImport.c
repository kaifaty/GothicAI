// Copyright © 2024. GothicKit Contributors
// SPDX-License-Identifier: MIT-Modern-Variant
#include "_Internal.h"

static void* DmLoader_resolveName(DmLoader* slf, const char* name, size_t* length) {
	if (mtx_lock(&slf->lock) != thrd_success) {
		return NULL;
	}

	void* bytes = NULL;
	for (size_t i = 0U; i < slf->resolvers.length; ++i) {
		DmResolver* resolver = &slf->resolvers.data[i];
		bytes = resolver->resolve(resolver->context, name, length);
		if (bytes != NULL) {
			break;
		}
	}

	(void) mtx_unlock(&slf->lock);
	return bytes;
}

static DmResult DmLoader_sourceSegment(void* ctx, char const* name, DmSegment** out) {
	*out = NULL;
	size_t length = 0;
	void* bytes = DmLoader_resolveName(ctx, name, &length);
	if (bytes == NULL) {
		return DmResult_NOT_FOUND;
	}
	DmSegment* value = NULL;
	DmResult rv = DmSegment_create(&value);
	if (rv != DmResult_SUCCESS) {
		Dm_free(bytes);
		return rv;
	}
	rv = DmSegment_parse(value, bytes, length);
	if (rv != DmResult_SUCCESS) {
		DmSegment_release(value);
		return rv;
	}
	*out = value;
	return DmResult_SUCCESS;
}

static DmResult DmLoader_sourceStyle(void* ctx, char const* name, DmStyle** out) {
	*out = NULL;
	size_t length = 0;
	void* bytes = DmLoader_resolveName(ctx, name, &length);
	if (bytes == NULL) {
		return DmResult_NOT_FOUND;
	}
	DmStyle* value = NULL;
	DmResult rv = DmStyle_create(&value);
	if (rv != DmResult_SUCCESS) {
		Dm_free(bytes);
		return rv;
	}
	rv = DmStyle_parse(value, bytes, length);
	if (rv != DmResult_SUCCESS) {
		DmStyle_release(value);
		return rv;
	}
	*out = value;
	return DmResult_SUCCESS;
}

static DmResult DmLoader_sourceDls(void* ctx, char const* name, DmDls** out) {
	*out = NULL;
	size_t length = 0;
	void* bytes = DmLoader_resolveName(ctx, name, &length);
	if (bytes == NULL) {
		return DmResult_NOT_FOUND;
	}
	DmDls* value = NULL;
	DmResult rv = DmDls_create(&value);
	if (rv != DmResult_SUCCESS) {
		Dm_free(bytes);
		return rv;
	}
	rv = DmDls_parse(value, bytes, length);
	if (rv != DmResult_SUCCESS) {
		DmDls_release(value);
		return rv;
	}
	*out = value;
	return DmResult_SUCCESS;
}

DmResult DmLoader_addResolver(DmLoader* slf, DmLoaderResolverCallback* resolve, void* ctx) {
	if (slf == NULL || resolve == NULL) {
		return DmResult_INVALID_ARGUMENT;
	}

	DmResolver resolver;
	resolver.context = ctx;
	resolver.resolve = resolve;

	if (mtx_lock(&slf->lock) != thrd_success) {
		return DmResult_MUTEX_ERROR;
	}

	DmResult rv = DmResolverList_add(&slf->resolvers, resolver);
	if (rv == DmResult_SUCCESS) {
		slf->source_factory.segment = DmLoader_sourceSegment;
		slf->source_factory.style = DmLoader_sourceStyle;
		slf->source_factory.dls = DmLoader_sourceDls;
	}

	(void) mtx_unlock(&slf->lock);
	return rv;
}
