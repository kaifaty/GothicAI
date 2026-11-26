#ifndef TREE_NANITE_ADAPTIVE_PAYLOAD_GLSL
#define TREE_NANITE_ADAPTIVE_PAYLOAD_GLSL

// A 32-lane tile and a two-word meshlet mask for each source face.
struct TreeTaskPayload { uint ids[32*64]; };
taskPayloadSharedEXT TreeTaskPayload data;

#endif
