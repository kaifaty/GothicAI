#ifndef GRASS_BLADE_LIMITS_GLSL
#define GRASS_BLADE_LIMITS_GLSL

// Shared by the shaders and the CPU bounds test.
const float GrassEarStalkWidth          = 0.45f; // cm
const float GrassEarWidth               = 1.5f;  // cm
const float GrassCullHorizontalReach    = 0.93f; // fraction of the grown height, before adding strip width
const float GrassCullHalfWidthScale     = 0.5f;  // the ear can extend by this fraction of its width vertically, too

#endif
