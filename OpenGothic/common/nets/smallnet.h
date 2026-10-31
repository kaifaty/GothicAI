#pragma once

#include <rapidjson/document.h>

#include <cstddef>
#include <initializer_list>
#include <map>
#include <string>
#include <vector>

namespace SmallNet {

struct Tensor final {
  std::vector<size_t> shape;
  std::vector<float> values;
  };

// One bounded JSON header line followed by contiguous little-endian float32
// tensors. Construction either validates the entire file or throws.
struct TensorFile final {
  explicit TensorFile(const std::string& path);
  const Tensor& tensor(const std::string& name, std::initializer_list<size_t> shape) const;

  rapidjson::Document header;
  std::map<std::string,Tensor> tensors;
  };

struct Linear final {
  size_t inputs = 0, outputs = 0;
  std::vector<float> weight, bias;
  };

std::vector<float> linear(const float* input, const Linear& layer, bool relu = false);
float sigmoid(float value);
// PyTorch GRUCell: reset/update/new, with the hidden new-gate bias inside reset.
std::vector<float> gruCell(const float* input, const std::vector<float>& hidden,
                           const Linear& inputLayer, const Linear& hiddenLayer);

}
