// The dense prefix maps a cluster to its resident tree record. A zero entry leaves the legacy path.
layout(binding = 13, std430) readonly buffer BoughData { uint boughData[]; };
#if !defined(LARGE)
layout(binding = 14, std430) buffer BoughState { uint boughState[]; };
#endif

uint boughEntry(uint clusterId) {
  return clusterId < boughData[0] ? boughData[4u+clusterId] : 0u;
  }
