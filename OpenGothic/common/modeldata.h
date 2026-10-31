#pragma once

#include "asset/modeltypes.h"

#include <filesystem>

namespace ModelData {

void writeHierarchy(const asset::ModelHierarchy& data, const std::filesystem::path& file);
asset::ModelHierarchy readHierarchy(const std::filesystem::path& file);
void writeMesh(const asset::ModelMesh& data, const std::filesystem::path& file,
               const std::vector<std::string>& attachmentOrder = {});
asset::ModelMesh readMesh(const std::filesystem::path& file);
void writeModel(const asset::Model& data, const std::filesystem::path& file,
                const std::vector<std::string>& attachmentOrder = {});
asset::Model readModel(const std::filesystem::path& file);
void writeAnimation(const asset::ModelAnimation& data, const std::filesystem::path& file,
                    const std::vector<asset::QuantizedAnimationSample>& encoded = {});
asset::ModelAnimation readAnimation(const std::filesystem::path& file);
void writeScript(const asset::ModelScript& data, const std::filesystem::path& file);
asset::ModelScript readScript(const std::filesystem::path& file);
void writeMorph(const asset::MorphMesh& data, const std::filesystem::path& file);
asset::MorphMesh readMorph(const std::filesystem::path& file);

std::string compare(const asset::ModelHierarchy& expected, const asset::ModelHierarchy& actual);
std::string compare(const asset::ModelMesh& expected, const asset::ModelMesh& actual);
std::string compare(const asset::Model& expected, const asset::Model& actual);
std::string compare(const asset::ModelAnimation& expected, const asset::ModelAnimation& actual);
std::string compare(const asset::ModelScript& expected, const asset::ModelScript& actual);
std::string compare(const asset::MorphMesh& expected, const asset::MorphMesh& actual);

}
