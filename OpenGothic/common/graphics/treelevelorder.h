#pragma once

namespace asset { struct StaticMesh; }

// The order of a level GLB of a tree with a bough hierarchy (reports/engine/2026-11-16-modern-trees/tree-bough-lod.md,
// «Лестница теней ближней ели»). The GLB stores the cards of its crown in no order of space, and
// PackedMesh::buildMeshlets starts a meshlet at the unused triangle of the lowest position: a meshlet takes cards
// from all over the crown, its sphere is the crown's, and every shadow page under the tree draws it. sortInSpace
// renumbers the positions piece by piece (triangles joined by a position: a card, a limb) along the Z-order curve
// of the pieces' middles, so a meshlet takes neighbours. No position, wedge or triangle changes otherwise: the
// colours of the wedges stay theirs, the level draws the same picture.
namespace TreeLevelOrder {

void sortInSpace(asset::StaticMesh& level);

}
