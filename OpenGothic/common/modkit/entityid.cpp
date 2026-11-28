#include "entityid.h"
#include "savearchive.h"

namespace modkit {

void IdGenerator::save(SaveArchive& fout) const {
  fout.write(SaveMagic, SaveVersion, nextEntity, nextItem);
  }

bool IdGenerator::load(SaveArchive& fin) {
  uint32_t magic = 0;
  uint16_t ver   = 0;
  uint64_t ent   = 1;
  uint64_t itm   = 1;
  fin.read(magic, ver, ent, itm);
  if(magic != SaveMagic || ver != SaveVersion)
    return false;
  nextEntity = ent > 0 ? ent : 1;
  nextItem   = itm > 0 ? itm : 1;
  return true;
  }

} // namespace modkit
