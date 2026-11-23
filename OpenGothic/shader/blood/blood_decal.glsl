#ifndef BLOOD_DECAL_GLSL
#define BLOOD_DECAL_GLSL

// One blood stain as a projected box (physics stage 8, step 4c; common/graphics/blooddecals.cpp).
struct BloodDecal {
  vec4 world;   // xyz: box centre in world space (cm)
  vec4 center;  // xyz: box centre relative to the large-world origin (scene.viewProjectLwcInv)
  vec4 axisU;   // xyz: along the stain (splash direction), half size
  vec4 axisV;   // xyz: surface normal, half depth of the box
  vec4 axisW;   // xyz: across the stain, half size
  vec4 uvRect;  // atlas tile: offset xy, size zw
  vec4 look;    // x: pool growth 0..1 (1: grown), y: wetness 0..1, z: dryness 0..1, w: opacity
  vec4 tint;    // rgb: fresh blood albedo (sRGB), a: soak into the ground 0..1
  };

#endif
