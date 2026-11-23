#ifndef GRASS_BLADE_LIMITS_GLSL
#define GRASS_BLADE_LIMITS_GLSL

// Shared by the shaders and the CPU bounds test.
const float GrassEarStalkWidth          = 0.45f; // cm
const float GrassEarWidth               = 1.5f;  // cm
const float GrassEarCardWidth           = 3.6f;  // cm: the ear's card seen close, its awns spread to the sides
const float GrassEarCardBase            = 1.4f;  // cm: the card where the ear starts
const float GrassEarNearFrom            = 300.f; // cm: nearer, the ear is its card whole
const float GrassEarNearTo              = 600.f; // cm: from here on the ear is the plain strip
const float GrassEarTopFar              = 0.93f; // of the height: the node where the ear is as wide as it gets
const float GrassEarTopNear             = 0.985f;// and where the card seen close is: it keeps its width nearly to the tip
const float GrassCullHorizontalReach    = 0.93f; // fraction of the grown height, before adding strip width
const float GrassCullHalfWidthScale     = 0.5f;  // the ear can extend by this fraction of its width vertically, too

#endif
