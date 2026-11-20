#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "graphics/grass/grassplace.h"
#include "worldchunkmanifest.h"

// Ground the content gives to the grass (reports/content/2026-11-19-crop-fields/stalk.md): the files
// worlds/<WORLD>/grass.d/*.json of the content, in the order of their names,
//   {"format": 1, "patches": [{"material": "OWN_CROP_STAND.TGA",
//     "positions": [x,y,z, ...], "uvs": [u,v, ...], "triangles": [i,j,k, ...]}]}
// positions: the world's cm, as a vob's; uvs: per vertex; material: a record of materials.json with `grass`. A
// triangle faces up whatever its winding. It belongs to the chunk its centre is in and is grass ground there as
// the landscape's own (Grass::addGrass), so it comes and goes with that chunk; a chunk the landscape does not have
// never takes its triangles. No device here: GrassField gives the slots.
namespace Grass {

class Plots final {
  public:
    // the field's slot of a material's name; uint16_t(-1): no record with `grass`
    using SlotOf = std::function<uint16_t(std::string_view material)>;

    // A file's text. False: the file is refused whole and `error` says why. True with an `error`: taken, less the
    // patches of a material without `grass`
    bool parse(std::string_view json, const SlotOf& slotOf, std::string& error);
    // Every file of worldContentDir/grass.d. A refused file or patch is one line of the log with the file's name
    void load(const std::filesystem::path& worldContentDir, const SlotOf& slotOf);
    // the chunk's triangles into its summary
    void addTo(Summary& out, ChunkId chunk) const;
    bool empty() const { return byChunk.empty(); }

  private:
    struct Tri final {
      float    pos[3][3] = {};
      float    uv[3][2]  = {};
      uint16_t slot      = 0;
      };
    std::map<std::pair<int32_t,int32_t>,std::vector<Tri>> byChunk;
  };

}
