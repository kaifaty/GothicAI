#pragma once

#include <Tempest/Signal>
#include <cstddef>
#include <zenkit/Vfs.hh>

#include "project/projectitem.h"

namespace zenkit
{
  class VfsNode;
};

class ProjectMgr {
  public:
    ProjectMgr();
    ~ProjectMgr();

    static ProjectMgr& inst();

    const zenkit::Vfs& vdfsIndex() const { return sources; }

    size_t       vdfCount() const;
    ProjectItem  vdf(size_t id);
    ProjectItem  root();

    ProjectItem  find(std::string_view name);

    Tempest::Signal<void()> onGpuAssetChanged;

  private:
    zenkit::Vfs sources;
    std::shared_ptr<ProjectItem::Data> mkIndex(const zenkit::VfsNode& node, size_t depth, const std::string& prefix);
    ProjectItem  find(const ProjectItem& itm, std::string_view name);

    std::shared_ptr<ProjectItem::Data> rootItem;
    std::shared_ptr<ProjectItem::Data> files;
  };
