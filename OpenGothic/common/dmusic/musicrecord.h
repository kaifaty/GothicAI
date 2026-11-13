#pragma once

#include "dlscollection.h"
#include "segment.h"
#include "style.h"

#include <cstdint>
#include <vector>

namespace MusicData {

// Dx8 projection codec: every parsed field memberwise, owning string lengths and raw UTF-16 units kept.
// Decoders require exact consumption and throw std::runtime_error("music data: <reason>").
std::vector<uint8_t>       encodeDx8(const Dx8::Segment& segment);
std::vector<uint8_t>       encodeDx8(const Dx8::Style& style);
std::vector<uint8_t>       encodeDx8(const Dx8::DlsCollection::Parsed& dls);
Dx8::Segment               decodeDx8Segment(const std::vector<uint8_t>& bin);
Dx8::Style                 decodeDx8Style(const std::vector<uint8_t>& bin);
Dx8::DlsCollection::Parsed decodeDx8Dls(const std::vector<uint8_t>& bin);

}
