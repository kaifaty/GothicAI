#pragma once

#include "font.h"
#include "staticmesh.h"
#include "modeltypes.h"
#include "worldmesh.h"
#include "worlddata.h"

namespace zenkit {
class Font;
class Material;
class Mesh;
class MultiResolutionMesh;
class SoftSkinMesh;
class ModelMesh;
class ModelHierarchy;
class ModelAnimation;
class ModelScript;
class MorphMesh;
class Model;
class Read;
class World;
class BspTree;
}

namespace asset {
Material fromZenKit(const zenkit::Material& source);
WorldMesh fromZenKit(const zenkit::Mesh& source);
StaticMesh fromZenKit(const zenkit::MultiResolutionMesh& source);
SoftSkinMesh fromZenKit(const zenkit::SoftSkinMesh& source);
ModelMesh fromZenKit(const zenkit::ModelMesh& source);
ModelHierarchy fromZenKit(const zenkit::ModelHierarchy& source);
ModelAnimation fromZenKit(const zenkit::ModelAnimation& source);
ModelScript fromZenKit(const zenkit::ModelScript& source);
MorphMesh fromZenKit(const zenkit::MorphMesh& source);
Model fromZenKit(const zenkit::Model& source);
Font fromZenKit(const zenkit::Font& source);
// Original MAN codes, before host-specific floating-point decoding.
// The reader retains its position on success.
std::vector<QuantizedAnimationSample> animationSamplesFromZenKit(zenkit::Read* source,
                                                               const zenkit::ModelAnimation& animation);
// Source insertion order, before the standard library orders the attachment map.
// The reader starts at a model-mesh section and retains its position on success.
std::vector<std::string> modelAttachmentOrder(zenkit::Read* source);

// Raw polygons belong to the BSP/archive boundary; the base stores these volumes.
std::vector<WorldData::Room> roomVolumesFromZenKit(const zenkit::Mesh& mesh, const zenkit::BspTree& bsp);
}

// Original archives only enter the engine through the import boundary.
namespace WorldData {
Data fromWorld(const zenkit::World& world, const WorldChunkSource& source);
// Returns false for worlds with a mesh. Unknown archive classes are errors.
bool loadZen(const std::vector<std::byte>& bytes, const WorldChunkSource& source, ZenFile& out);
// P14: a ZenGin cutscene library (OU.BIN/OU.DAT) as kind "cutscene_library": the zCCSLib root is read
// like zenkit::CutsceneLibrary::load but without its sort, so the roots keep archive order. Converted with
// the shared graph copy and checked back; then the bytes are parsed again with ZenKit's own reader and
// std::sort of the roots by name must give exactly CutsceneLibrary::blocks. Throws on a root that is not
// zCCSLib, a null root and any parse or comparison error.
void loadCutsceneLibrary(const std::vector<std::byte>& bytes, const WorldChunkSource& source, ZenFile& out);
// Converter contract, including all supported archive classes and graph references.
void selfTest(const std::filesystem::path& dir);
}
