#ifndef GRASS_BORDER_GLSL
#define GRASS_BORDER_GLSL

// The border of a meadow with a ground that has `verge` (reports/engine/2026-11-14-meadow-ground): the pieces of
// the grass's open edges along it, found from the chunks' meshes as they are read and packed by Grass::packBorder
// (common/graphics/grass/grassborder.h). A head of 4 words (the table's side, 0 with no piece at all; the bin, cm),
// the table of side*side words wrapped round the world (offset << 6 | count), then 5 words a piece. Bound in the
// landscape's G-buffer pass with bindless textures alone (DrawCommands::setBindings).
const uint L_GrassBorder = 26;
layout(binding = L_GrassBorder, std430) readonly buffer GrassBorderBuf { uint grassBorder[]; };

// The nearest piece within `limit` of a place, cm (not over Grass::BorderPad): Grass::nearestBorder
bool grassBorderNear(vec2 p, float limit, out float dist, out float reach, out float margin) {
  dist   = limit;
  reach  = 0.0;
  margin = 0.0;
  const uint side = grassBorder[0];
  if(side==0u)
    return false;
  const ivec2 c = ivec2(floor(p/float(grassBorder[1])));
  const uint  e = grassBorder[4u + (uint(c.y) & (side-1u))*side + (uint(c.x) & (side-1u))];
  bool found = false;
  uint at    = e >> 6;
  for(uint i=0u; i<(e & 63u); ++i, at+=5u) {
    const vec2  a  = vec2(uintBitsToFloat(grassBorder[at]),    uintBitsToFloat(grassBorder[at+1u]));
    const vec2  ab = vec2(uintBitsToFloat(grassBorder[at+2u]), uintBitsToFloat(grassBorder[at+3u])) - a;
    const vec2  ap = p - a;
    const float d  = length(ap - ab*clamp(dot(ap,ab)/max(dot(ab,ab),1e-6), 0.0, 1.0));
    if(d<dist) {
      const uint w = grassBorder[at+4u];
      dist   = d;
      reach  = float(w & 0xFFu);
      margin = float((w >> 8) & 0xFFu);
      found  = true;
      }
    }
  return found;
  }

#endif
