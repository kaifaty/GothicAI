#version 450

#extension GL_GOOGLE_include_directive    : enable
#extension GL_ARB_separate_shader_objects : enable

#include "virtual_shadow/vsm_common.glsl"

// Clears the atlas regions the frame draws under the lamp-face cache (VsmClearCmd): a quad an entry, at depth 0
// (nothing there), over first page .. + size pages; the depth test is off (Shaders::vsmClearPages)
layout(binding = 0, std430) readonly buffer Clr { VsmClearCmd clearCmd; uint clearList[]; };

void main() {
  if(gl_InstanceIndex>=VSM_CLEAR_MAX) {
    gl_Position = vec4(0);
    return;
    }
  const uint  e    = clearList[gl_InstanceIndex];
  const vec2  at   = vec2(unpackVsmPageId(e & 0xFFFFu));
  const float size = float(e >> 16u);
  const vec2  q[6] = {vec2(0,0), vec2(1,0), vec2(0,1), vec2(0,1), vec2(1,0), vec2(1,1)};

  const vec2 pos = (at + q[gl_VertexIndex]*size)/VSM_PAGE_PER_ROW;
  gl_Position = vec4(pos*2.0-1.0, 0.0, 1.0);
  }
