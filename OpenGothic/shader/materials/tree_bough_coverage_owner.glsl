uint crownEntry(uint id) {
  uint entry = 0u;
  if((id >> 30u)==1u) {
    const uint instance = id & 0x3fffffffu;
    if(instance<data[3])
      entry = data[data[2]+instance];
    }
  else if(id!=0u && id<=data[0]) {
    entry = data[4u+id-1u];
    }
  return entry;
  }
