#version 450
#extension GL_ARB_separate_shader_objects : enable
#extension GL_GOOGLE_include_directive : enable

#include "scene.glsl"
#include "common.glsl"
#include "blood/blood_decal.glsl"

// Blood on the static GBuffer (physics stage 8, step 4c). The box is projected onto the depth of the
// scene: albedo (RGB only: its alpha holds the material hint bits) and, with material maps, roughness.
// Wet blood is dark and glossy; it dries matte and turns brown. Research and targets:
// reports/physics/2026-11-02-blood/README.md.

layout(binding = 0, std140) uniform UboScene {
  SceneDesc scene;
  };
layout(binding = 1) uniform sampler2D  depth;
layout(binding = 2) uniform usampler2D gbufNormal;
layout(binding = 3) uniform sampler2D  atlas;
layout(binding = 4, std430) readonly buffer Decals { BloodDecal decal[]; };

layout(location = 0) in flat uint inId;

layout(location = 0) out vec4 outDiffuse;
#if defined(MATERIAL_MAPS)
layout(location = 1) out vec4 outOrm;
#endif

void main() {
  const ivec2 fc = ivec2(gl_FragCoord.xy);
  const float z  = texelFetch(depth, fc, 0).r;
  if(z>=1.0)
    discard;

  const BloodDecal d = decal[inId];
  const vec2 ndc = (gl_FragCoord.xy*scene.screenResInv)*2.0 - vec2(1.0);
  const vec4 p4  = scene.viewProjectLwcInv*vec4(ndc, z, 1.0);
  const vec3 p   = p4.xyz/p4.w - d.center.xyz;

  const vec3 l = vec3(dot(p,d.axisU.xyz)/dot(d.axisU.xyz,d.axisU.xyz),
                      dot(p,d.axisV.xyz)/dot(d.axisV.xyz,d.axisV.xyz),
                      dot(p,d.axisW.xyz)/dot(d.axisW.xyz,d.axisW.xyz));
  if(any(greaterThan(abs(l), vec3(1.0))))
    discard;

  // no stretching on steep faces, no blood on the far side of thin geometry
  const vec3  n      = normalFetch(gbufNormal, fc);
  const float facing = dot(n, normalize(d.axisV.xyz));
  const float fade   = smoothstep(0.45, 0.75, facing) * (1.0 - smoothstep(0.55, 1.0, abs(l.y)));

  const vec2 uv = d.uvRect.xy + (l.xz*0.5+0.5)*d.uvRect.zw;
  const vec4 t  = texture(atlas, uv);
  const float shape  = t.r;
  float       thick  = t.g;
  const float spread = t.b; // pools: when the edge arrives, 0 at the centre .. 1 at the final edge

  // a pool spreads: covered where the front has passed, thin right behind the front
  const float growth = d.look.x;
  const float front  = 1.0 - smoothstep(growth-0.012, growth+0.012, spread);
  thick *= clamp((growth-spread)*5.0, 0.0, 1.0)*0.7 + 0.3*front;

  const float soak = d.tint.a;
  const float a    = shape*front*fade*d.look.w*(1.0-0.25*soak);
  if(a<=0.002)
    discard;

  // colour: thin blood is red, thick is near black-red; drying turns it brown; soil drinks it darker
  const vec3 fresh = d.tint.rgb;
  vec3 col = mix(fresh, fresh*vec3(0.42,0.30,0.32), thick);
  col = mix(col, vec3(0.13,0.055,0.04)*(1.0-0.4*thick), d.look.z);
  col *= 1.0 - 0.35*soak;
  outDiffuse = vec4(col, a);

#if defined(MATERIAL_MAPS)
  // wet: a mirror-like film where it is thick; soaked or dried blood is matte
  const float wet   = d.look.y*(0.35+0.65*thick)*(1.0-soak);
  const float rough = mix(0.72, 0.06, wet);
  outOrm = vec4(1.0, rough, 0.0, a);
#endif
  }
