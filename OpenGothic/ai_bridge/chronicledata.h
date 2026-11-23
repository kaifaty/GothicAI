#pragma once

#include <string_view>

#include "game/chronicle.h"

// Display text is converted to the game codepage; identifiers and paths retain UTF-8.
ChronicleData parseChronicle(std::string_view json);
FateCard parseFateCard(std::string_view json);
