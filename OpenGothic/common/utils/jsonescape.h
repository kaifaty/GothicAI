#pragma once

#include <string>
#include <string_view>

// Quote JSON text, preserving non-control bytes (including UTF-8) verbatim.
std::string jsonEscape(std::string_view text);
