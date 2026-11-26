#ifndef TREE_NANITE_CONTOUR_TILES_GLSL
#define TREE_NANITE_CONTOUR_TILES_GLSL

layout(binding = 21, std430) readonly buffer TreeCut { uint groupCuts[]; };

const uint ContourTileSize = 32u;

bool contourTileRange(uint tile, uint groupCount,
                      out uint slot, out uint firstFace, out uint endFace, out uint level) {
  const uint tiles = (frame.contourControl.y + ContourTileSize - 1u) / ContourTileSize;
  slot = tile / tiles;
  if(slot >= frame.control.w)
    return false;
  const uint activeLevel = groupCuts[slot * (groupCount + 1u)];
  if(activeLevel <= 1u)
    return false;

  level = activeLevel - 1u;
  firstFace = frame.contourOffsets[level - 1u] + (tile % tiles) * ContourTileSize;
  endFace = level == 4u ? frame.contourControl.x : frame.contourOffsets[level];
  return firstFace < endFace;
  }

uint contourStamp(uint level, uint serial) {
  return ((level - 1u) << 30u) | serial;
  }

#endif
