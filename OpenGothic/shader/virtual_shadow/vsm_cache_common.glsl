#ifndef VSM_CACHE_COMMON_GLSL
#define VSM_CACHE_COMMON_GLSL

// A lamp's state in the lamp-face cache, kept from frame to frame (Renderer::vsm.cache, zero: an empty cache):
// its position and shadow range as last seen, its six faces' entries (vsm_cache.glsl) and the faces this frame
// draws again because a shadow caster changed in them (a bit a face)
struct VsmCacheLight {
  vec4 prev;
  uint face[6];
  uint dirty;
  uint padding0;
  };

#endif
