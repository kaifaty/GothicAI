#pragma once

#include <filesystem>

void cookTreeNanite(const std::filesystem::path& geometry, const std::filesystem::path& materials,
                    const std::filesystem::path& output, const std::filesystem::path& rig = {});
