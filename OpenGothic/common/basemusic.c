#include "basemusic.h"

#include "_Internal.h"

#include <string.h>

static void BaseMusic_visitBand(DmBand const* band, size_t message, size_t index,
                                BaseMusicVisit* visit, void* ctx) {
  for(size_t i = 0; i < band->instruments_len; ++i) {
    DmInstrument const* ins = &band->instruments[i];
    BaseMusicReference r;
    memset(&r, 0, sizeof r);
    r.message      = message;
    r.band         = index;
    r.instrument   = i;
    r.file         = ins->reference.file;
    r.name         = ins->reference.name;
    r.options      = (uint32_t) ins->options;
    r.downloadable = (ins->options & (DmInstrument_VALID_PATCH | DmInstrument_VALID_BANKSELECT)) != 0;
    r.object       = ins->dls;
    memcpy(r.guid, ins->reference.guid.data, sizeof r.guid);
    if(ins->dls != NULL)
      memcpy(r.object_guid, ins->dls->guid.data, sizeof r.object_guid);
    visit(ctx, &r);
    }
  }

int BaseMusic_trace(char const* root, BaseMusicResolve* resolve, void* resolveCtx,
                    BaseMusicVisit* visit, void* visitCtx, int* loaded) {
  *loaded = 0;
  DmLoader* loader = NULL;
  DmResult rv = DmLoader_create(&loader, 0);
  if(rv != DmResult_SUCCESS)
    return (int) rv;
  rv = DmLoader_addResolver(loader, resolve, resolveCtx);
  DmSegment* sgt = NULL;
  if(rv == DmResult_SUCCESS)
    rv = DmLoader_getSegment(loader, root, &sgt);
  if(rv != DmResult_SUCCESS) {
    DmLoader_release(loader);
    return (int) rv;
    }
  // DmLoader_getSegment with DmLoader_DOWNLOAD continues exactly so; keeping the segment
  // after a failed download lets the trace record what stayed unresolved.
  rv = DmSegment_download(sgt, loader);
  *loaded = 1;

  for(size_t m = 0; m < sgt->messages.length; ++m) {
    DmMessage const* msg = &sgt->messages.data[m];
    if(msg->type == DmMessage_BAND && msg->band.band != NULL) {
      BaseMusic_visitBand(msg->band.band, m, SIZE_MAX, visit, visitCtx);
      }
    else if(msg->type == DmMessage_STYLE) {
      BaseMusicReference r;
      memset(&r, 0, sizeof r);
      r.style   = 1;
      r.message = m;
      r.file    = msg->style.reference.file;
      r.name    = msg->style.reference.name;
      r.object  = msg->style.style;
      memcpy(r.guid, msg->style.reference.guid.data, sizeof r.guid);
      if(msg->style.style != NULL)
        memcpy(r.object_guid, msg->style.style->guid.data, sizeof r.object_guid);
      visit(visitCtx, &r);
      if(msg->style.style != NULL)
        for(size_t b = 0; b < msg->style.style->bands.length; ++b)
          BaseMusic_visitBand(msg->style.style->bands.data[b], m, b, visit, visitCtx);
      }
    }

  DmSegment_release(sgt);
  DmLoader_release(loader);
  return (int) rv;
  }
