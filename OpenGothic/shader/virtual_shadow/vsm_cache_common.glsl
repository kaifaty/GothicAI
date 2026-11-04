#ifndef VSM_CACHE_COMMON_GLSL
#define VSM_CACHE_COMMON_GLSL

// A lamp's state in the lamp-face cache, kept from frame to frame (Renderer::vsm.cache, zero: an empty cache):
// where its faces were drawn (position, shadow range), its six faces' entries (vsm_cache.glsl), and a bit a face:
// a static caster changed in it (dirty), a movable caster is in it now or was a frame ago (moving)
struct VsmCacheLight {
  vec4 prev;
  uint face[6];
  uint dirty;
  uint moving;
  };

// lamp faces drawn whole this frame, to store their static part (vsm_cache_store.comp, an indirect dispatch: a
// workgroup a face); entry = first page | size in pages << 16
struct VsmStoreCmd {
  uint groupsX;
  uint groupsY;
  uint groupsZ;
  uint padding0;
  };

#endif
