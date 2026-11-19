#pragma once

#include <string_view>

namespace ClusterLodRender {

// TestBench can capture the same camera with the graph's base cut, the normal cut or level colors.
void setBenchView(std::string_view mode);
// Reuse the terrain mask capture with only own static mesh replacements.
void setBenchOwnMeshMask(bool enabled);
bool benchOwnMeshMask();

}
