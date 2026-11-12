#include "savearchive.h"

#include <cstring>
#include <exception>

#include <Tempest/MemReader>
#include <Tempest/MemWriter>

size_t SaveArchive::writeFunc(void* pOpaque, uint64_t file_ofs, const void* pBuf, size_t n) {
  auto& self = *reinterpret_cast<SaveArchive*>(pOpaque);
  file_ofs += mz_zip_get_archive_file_start_offset(&self.impl);
  if(file_ofs!=self.curOffset) {
    self.impl.m_last_error = MZ_ZIP_FILE_SEEK_FAILED;
    return 0;
    }
  size_t ret = self.fout->write(pBuf,n);
  self.curOffset+=ret;
  return ret;
  }

size_t SaveArchive::readFunc(void* pOpaque, uint64_t file_ofs, void* pBuf, size_t n) {
  auto& self = *reinterpret_cast<SaveArchive*>(pOpaque);
  file_ofs += mz_zip_get_archive_file_start_offset(&self.impl);
  if(self.curOffset<file_ofs) {
    size_t diff = size_t(file_ofs-self.curOffset);
    size_t mv   = self.fin->seek(diff);
    self.curOffset += uint64_t(mv);
    if(diff!=mv) {
      self.impl.m_last_error = MZ_ZIP_FILE_SEEK_FAILED;
      return 0;
      }
    }
  if(self.curOffset>file_ofs) {
    size_t diff = size_t(self.curOffset-file_ofs);
    size_t mv   = self.fin->unget(diff);
    self.curOffset -= uint64_t(mv);
    if(diff!=mv) {
      self.impl.m_last_error = MZ_ZIP_FILE_SEEK_FAILED;
      return 0;
      }
    }
  size_t ret = self.fin->read(pBuf,n);
  self.curOffset+=ret;
  return ret;
  }

SaveArchive::SaveArchive(Tempest::ODevice& fout) : fout(&fout) {
  entryBuf .reserve(1*1024*1024);
  entryName.reserve(256);

  impl.m_pWrite           = SaveArchive::writeFunc;
  impl.m_pIO_opaque       = this;
  impl.m_zip_type         = MZ_ZIP_TYPE_USER;
  if(!mz_zip_writer_init_v2(&impl, 0, 0))
    throw std::runtime_error("unable to create game archive");
  }

SaveArchive::SaveArchive(Tempest::IDevice& fin) : fin(&fin) {
  entryBuf .resize(1*1024*1024);
  entryName.reserve(256);

  impl.m_pRead            = SaveArchive::readFunc;
  impl.m_pIO_opaque       = this;
  impl.m_zip_type         = MZ_ZIP_TYPE_USER;
  if(!mz_zip_reader_init(&impl, fin.size(), 0))
    throw std::runtime_error("unable to read game archive");
  }

SaveArchive::~SaveArchive() noexcept(false) {
  if(fin!=nullptr)
    mz_zip_reader_end(&impl);
  if(fout==nullptr)
    return;
  try {
    closeEntry();
    if(!mz_zip_writer_finalize_archive(&impl) || !fout->flush())
      throw std::runtime_error("unable to finalize game archive");
    }
  catch(...) {
    mz_zip_writer_end(&impl);
    if(std::uncaught_exceptions()==0)
      throw;
    return;
    }
  mz_zip_writer_end(&impl);
  }

void SaveArchive::closeEntry() {
  if(fout==nullptr)
    return;
  if(entryBuf.empty())
    return;

  mz_uint level  = entryBuf.size()>256 ? MZ_BEST_SPEED : MZ_NO_COMPRESSION;
  mz_bool status = mz_zip_writer_add_mem(&impl, entryName.c_str(), entryBuf.data(), entryBuf.size(), level);
  entryBuf .clear();
  entryName.clear();
  if(!status)
    throw std::runtime_error("unable to write entry in game archive");
  }

bool SaveArchive::implSetEntry(std::string_view fname) {
  size_t prefix = 0;
  if(fout!=nullptr) {
    while(prefix<fname.size() && prefix<entryName.size()) {
      if(entryName[prefix]!=fname[prefix])
        break;
      ++prefix;
      }
    }
  closeEntry();
  entryName = fname;
  if(fout!=nullptr) {
    for(size_t i=prefix; i<entryName.size(); ++i) {
      if(entryName[i]=='/' && i+1<entryName.size()) {
        const char prev = entryName[i+1];
        entryName[i+1] = '\0';
        const auto it = outFileList.insert(entryName.c_str());
        if(it.second) {
          mz_bool status = mz_zip_writer_add_mem(&impl, entryName.c_str(), NULL, 0, MZ_NO_COMPRESSION);
          if(!status)
            throw std::runtime_error("unable to allocate entry in game archive");
          }
        entryName[i+1] = prev;
        }
      }
    return true;
    }
  if(fin!=nullptr) {
    mz_uint32 id = mz_uint32(-1);
    if(mz_zip_reader_locate_file_v2(&impl, entryName.c_str(), nullptr, 0, &id)) {
      mz_zip_archive_file_stat stat = {};
      mz_zip_reader_file_stat(&impl,id,&stat);
      entryBuf.resize(size_t(stat.m_uncomp_size));
      mz_zip_reader_extract_file_to_mem(&impl,entryName.c_str(),entryBuf.data(),entryBuf.size(),0);
      } else {
      entryBuf.clear();
      }
    readOffset = 0;
    return !entryBuf.empty();
    }
  return false;
  }

uint32_t SaveArchive::implDirectorySize(std::string_view e) {
  uint32_t count = 0;
  std::vector<char> filename;
  filename.reserve(256);
  for(mz_uint i=0; i<mz_zip_reader_get_num_files(&impl); ++i) {
    const auto length = mz_zip_reader_get_filename(&impl, i, nullptr, 0);
    if(length==0)
      throw std::runtime_error("unable to locate entry in game archive");
    filename.resize(length);
    if(mz_zip_reader_get_filename(&impl, i, filename.data(), length)!=length)
      throw std::runtime_error("unable to read entry name in game archive");
    std::string_view name(filename.data(), length-1);
    if(name.size()<=e.size() || !name.starts_with(e))
      continue;
    const auto child = name.substr(e.size());
    const auto separator = child.find('/');
    if(separator==std::string_view::npos || separator+1==child.size())
      ++count;
    }
  return count;
  }

void SaveArchive::writeBytes(const void* buf, size_t sz) {
  if(sz==0)
    return;
  size_t at = entryBuf.size();
  entryBuf.resize(entryBuf.size()+sz);
  std::memcpy(&entryBuf[at],buf,sz);
  }

void SaveArchive::readBytes(void* buf, size_t sz) {
  if(fin==nullptr || readOffset+sz>entryBuf.size())
    throw std::runtime_error("unable to read save-game file");
  std::memcpy(buf,&entryBuf[size_t(readOffset)],sz);
  readOffset+=sz;
  }

void SaveArchive::implWrite(const std::string& s) {
  uint32_t sz=uint32_t(s.size());
  implWrite(sz);
  writeBytes(s.data(),sz);
  }

void SaveArchive::implRead(std::string& s) {
  uint32_t sz=0;
  implRead(sz);
  s.resize(sz);
  if(sz>0)
    readBytes(&s[0],sz);
  }

void SaveArchive::implWrite(std::string_view s) {
  uint32_t sz=uint32_t(s.size());
  implWrite(sz);
  writeBytes(s.data(),sz);
  }

void SaveArchive::implWrite(const Tempest::Pixmap& p) {
  implWrite(p, "png");
  }

void SaveArchive::implWrite(const Tempest::Pixmap& p, const char* ext) {
  std::vector<uint8_t> tmp;
  tmp.reserve(4*1024*1024);
  Tempest::MemWriter w{tmp};
  p.save(w, ext);
  writeBytes(tmp.data(),tmp.size());
  }

void SaveArchive::implRead(Tempest::Pixmap& p) {
  Tempest::MemReader r{&entryBuf[size_t(readOffset)],size_t(entryBuf.size()-readOffset)};
  p = Tempest::Pixmap(r);
  readOffset += r.cursorPosition();
  }

void SaveArchive::implWrite(const asset::AnimationSample& i) {
  writeBytes(&i,sizeof(i));
  }

void SaveArchive::implRead(asset::AnimationSample& i) {
  if (version() < 40) {
    read(i.position.x,i.position.y,i.position.z);
    read(i.rotation.x,i.rotation.y,i.rotation.z,i.rotation.w);
    } else {
    readBytes (&i,sizeof(i));
    }
  }
