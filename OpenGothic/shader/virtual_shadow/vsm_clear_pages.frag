#version 450

#extension GL_GOOGLE_include_directive    : enable
#extension GL_ARB_separate_shader_objects : enable
#extension GL_EXT_samplerless_texture_functions : enable

#include "virtual_shadow/vsm_common.glsl"
#define VSM_CACHE_NO_PLACEMENT
#include "virtual_shadow/vsm_cache.glsl"

// depth 0, or a composed lamp face's static part from the store (vsm_cache_store.comp)
layout(binding = 1) uniform texture2D store;

layout(location = 0) in flat uint copy;

void main() {
  if(copy==0) {
    gl_FragDepth = 0;
    return;
    }
  const ivec2 org = ivec2(0, int(VSM_CACHE_FIRST_ROW)*VSM_PAGE_SIZE);
  gl_FragDepth = texelFetch(store, ivec2(gl_FragCoord.xy) - org, 0).x;
  }
