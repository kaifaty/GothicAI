#include "basestoreimport.h"
#include "videodata.h"

BaseStore::Selection BaseStore::ensure(const std::filesystem::path& gothic,
                                      const BaseImport::Files& mounted, const BaseImport::Progress& log) {
  return resolve(gothic,mounted,log,[&](const auto& out) { BaseImport::build(gothic,out,log); });
  }

BaseStore::Selection BaseStore::ensureVideos(const std::filesystem::path& gothic,
                                            const BaseImport::Progress& log,
                                            std::shared_ptr<std::atomic_bool> cancelled, size_t workers) {
  return resolveVideos(gothic,{},log,cancelled,
    [&](const auto& out) { VideoData::build(gothic,out,log,cancelled.get(),workers); },
    [&] { return VideoData::identify(gothic,cancelled.get()); });
  }
