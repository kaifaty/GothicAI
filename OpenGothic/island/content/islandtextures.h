#pragma once

// Textures of the empty island, drawn in code (reports/engine/2026-11-13-empty-island/island-content.md): the maps
// of every material with their materials.json, and the sky. Every pattern is a function of the tile's (u, v) over
// lattices that wrap at the tile's edge, so a tile meets itself without a seam.

#include <filesystem>
#include <string>
#include <vector>

namespace island {

// Materials of the props (islandprops.h); the ground's are groundName() of islandterrain.h.
constexpr char BoulderMaterial[] = "ISLAND_BOULDER"; // the rock's maps by world position, for loose stones
constexpr char WoodMaterial[]    = "ISLAND_WOOD";

struct MaterialNote {
  std::string name;          // the key of materials.json, without ".TGA"
  float       luminance = 0; // mean linear albedo
  float       spread    = 0; // how far the mean of a quarter of the tile's side strays from it, as a share
  };

// materials.json and the DDS maps under dir (<island>/content/materials). Returns what was measured.
std::vector<MaterialNote> writeMaterials(const std::filesystem::path& dir);

// Clouds of the day and the night, the sun and the moon under dir (<island>/textures), by the names the sky asks for.
void writeSky(const std::filesystem::path& dir);

// Names the sky asks Resources for.
inline const char* const SkyTextures[] = {"SKYDAY_LAYER0_A0.TGA", "SKYDAY_LAYER1_A0.TGA", "SKYNIGHT_LAYER0_A0.TGA",
                                          "SKYNIGHT_LAYER1_A0.TGA", "UNSUN5.TGA", "MOON.TGA"};

}
