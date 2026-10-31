#pragma once

#include "asset/font.h"

#include <filesystem>
#include <string>
#include <string_view>

// P11 font records (docs/asset-migration.md): <NAME>.FNT.json with the metadata and the
// fixed sibling <NAME>.FNT.bin with the glyphs, little-endian, raw float bits.
namespace FontData {

// "FONT_X.FNT" -> "FONT_X.TGA": the texture Resources::implLoadFont pairs with that FNT,
// the same requested stem with ".tga". The name must end with ".FNT" (any case).
std::string atlasName(std::string_view fnt);
void        writeFont(const asset::Font& font, std::string_view atlas, const std::filesystem::path& file);
// Rejects a missing, corrupt or unsupported record with std::runtime_error.
asset::Font readFont(const std::filesystem::path& file, std::string* atlas = nullptr);
// Empty when equal: name bytes, height, glyph order, widths and UV float bits.
std::string compare(const asset::Font& expected, const asset::Font& actual);

}
