#pragma once

#include <cstdint>
#include <vector>

#include "asset/staticmesh.h"

// COLOR_0 of an own GLB: data for the vertex shader (the tree's wind, reports/engine/2026-11-16-modern-trees), not
// a tint. Its four bytes go to Resources::Vertex::color as they are, R in the low byte, with no gamma; RGB without
// alpha gets alpha one.
//
// A second read of the bytes loadGltfMesh took, beside it and not in it: gltfmesh.cpp and asset/staticmesh.h are
// inputs of the base's fingerprint (cmake/BaseFormat.cmake), and a colour of the drawn vertex is not a part of the
// base. So this file walks the scene in loadGltfMesh's order (gltfmesh.cpp: loadScene, Converter::addPrimitive) and
// must follow it: the nodes off a stack, a sub-mesh a material by its first use, one wedge an element of POSITION,
// primitives of one node and material with the same POSITION, NORMAL and TEXCOORD_0 share their wedges. The counts
// are checked against the mesh, so a walk out of step throws and never paints a wrong vertex.

// one list a sub-mesh, one colour a wedge
using GltfMeshColors = std::vector<std::vector<uint32_t>>;

// glb: the bytes loadGltfMesh returned for `mesh`. Empty when no primitive carries COLOR_0: the vertices keep their
// ones. Throws std::runtime_error on a COLOR_0 that is not float or normalized unsigned RGB or RGBA, on primitives
// that share their wedges and differ in COLOR_0, and on bytes that are not of this mesh.
GltfMeshColors loadGltfMeshColors(const std::vector<uint8_t>& glb, const asset::StaticMesh& mesh);
