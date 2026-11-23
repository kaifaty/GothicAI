#pragma once

#include <Tempest/IDevice>
#include <Tempest/ODevice>
#include <Tempest/Pixmap>

#include <Tempest/Matrix4x4>

#include <vector>
#include <unordered_set>
#include <cstdint>
#include <type_traits>
#include <tuple>
#include <string>
#include <string_view>

#include <miniz.h>

#include "asset/modeltypes.h"

#include "utils/string_frm.h"

class SaveArchive {
  public:
    enum Version : uint16_t {
      Current    = 55,
      MinVersion = 36,

      Last_2025  = 53,
      Last_2024  = 49,
      Last_2023  = 46,
      Last_2022  = 41,
      Last_2021  = 36,
      First      = 36, // first zip-based version
      };
    SaveArchive(Tempest::ODevice& fout);
    SaveArchive(Tempest::IDevice&  fin);
    SaveArchive(SaveArchive&&)=delete;
    ~SaveArchive() noexcept(false);

    uint16_t version()              const { return wldVer; }
    void     setVersion(uint16_t v)       { wldVer = v;    }
    uint16_t globalVersion()        const { return curVer; }
    void     setGlobalVersion(uint16_t v) { curVer = v;    }
    std::string_view worldName() const   { return wldName; }

    template<class ... Args>
    bool setEntry(const Args& ... args) {
      string_frm s(args...);
      return implSetEntry(s);
      }

    template<class ... Args>
    uint32_t directorySize(const Args& ... args) {
      string_frm s(args...);
      return implDirectorySize(s);
      }

    // raw
    void writeBytes(const void* v,size_t sz);
    void readBytes (void* v,size_t sz);

    template<class ... Arg>
    void write(const Arg& ... a){
      (writeValue(*this,a),... );
      }

    template<class ... Arg>
    void read(Arg& ... a){
      (readValue(*this,a),... );
      }

  protected:
    std::string_view wldName = "_";

    // trivial types
    void implWrite(bool      i) { implWrite(uint8_t(i ? 1 : 0)); }
    void implRead (bool&     i) { uint8_t x=0; read(x); i=(x!=0); }

    void implWrite(char      i) { writeBytes(&i,sizeof(i)); }
    void implRead (char&     i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint8_t   i) { writeBytes(&i,sizeof(i)); }
    void implRead (uint8_t&  i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint16_t  i) { writeBytes(&i,sizeof(i)); }
    void implRead (uint16_t& i) { readBytes (&i,sizeof(i)); }

    void implWrite(int32_t   i) { writeBytes(&i,sizeof(i)); }
    void implRead (int32_t&  i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint32_t  i) { writeBytes(&i,sizeof(i)); }
    void implRead (uint32_t& i) { readBytes (&i,sizeof(i)); }

    void implWrite(uint64_t  i) { writeBytes(&i,sizeof(i)); }
    void implRead (uint64_t& i) { readBytes (&i,sizeof(i)); }

    void implWrite(float  i)    { writeBytes(&i,sizeof(i)); }
    void implRead (float& i)    { readBytes (&i,sizeof(i)); }

    template<class T, std::enable_if_t<!std::is_same<T,uint32_t>::value && !std::is_same<T,uint64_t>::value && std::is_same<T,size_t>::value,bool> = true>
    void implWrite(T ) = delete;
    template<class T, std::enable_if_t<!std::is_same<T,uint32_t>::value && !std::is_same<T,uint64_t>::value && std::is_same<T,size_t>::value,bool> = true>
    void implRead(T&) = delete;

    // composite types
    void implWrite(const Tempest::Vec3& s) { writeBytes(&s,sizeof(s)); }
    void implRead (Tempest::Vec3& s)       { readBytes (&s,sizeof(s)); }

    void implWrite(const Tempest::Matrix4x4& i) { writeBytes(&i,sizeof(i)); }
    void implRead (Tempest::Matrix4x4& i)       { readBytes (&i,sizeof(i)); }

    void implWrite(const asset::AnimationSample& i);
    void implRead (asset::AnimationSample& i);

    // strings
    void implWrite(const std::string&              s);
    void implRead (std::string&                    s);

    void implWrite(std::string_view                s);

    // Retain the concrete archive when dispatching elements of game-type containers.
    template<class Archive, class T>
    static void writeValue(Archive& archive, const T& value) {
      archive.implWrite(value);
      }

    template<class Archive, class T>
    static void readValue(Archive& archive, T& value) {
      archive.implRead(value);
      }

    // vectors
    template<class Archive, class T>
    static void writeValue(Archive& archive, const std::vector<T>& s) {
      implWriteVec(archive,s,std::is_trivial<T>());
      }

    template<class Archive, class T>
    static void readValue(Archive& archive, std::vector<T>& s){
      implReadVec(archive,s,std::is_trivial<T>());
      }

    template<class Archive>
    static void writeValue(Archive& archive, const std::vector<bool>& s) {
      uint32_t sz=uint32_t(s.size());
      archive.write(sz);
      for(size_t i=0; i<s.size(); ++i)
        archive.write(s[i]);
      }

    template<class Archive>
    static void readValue(Archive& archive, std::vector<bool>& s) {
      uint32_t sz=0;
      archive.read(sz);
      s.resize(sz);
      for(size_t i=0; i<s.size(); ++i) {
        bool b=false;
        archive.read(b);
        s[i] = b;
        }
      }

    template<class Archive, class T>
    static void implWriteVec(Archive& archive, const std::vector<T>& s,std::false_type) {
      uint32_t sz=uint32_t(s.size());
      archive.write(sz);
      for(auto& i:s)
        archive.write(i);
      }

    template<class Archive, class T>
    static void implWriteVec(Archive& archive, const std::vector<T>& s,std::true_type) {
      uint32_t sz=uint32_t(s.size());
      archive.write(sz);
      archive.writeBytes(s.data(),sz*sizeof(T));
      }

    template<class Archive, class T>
    static void implReadVec(Archive& archive, std::vector<T>& s,std::false_type) {
      uint32_t sz=0;
      archive.read(sz);
      s.resize(sz);
      for(auto& i:s)
        archive.read(i);
      }

    template<class Archive, class T>
    static void implReadVec(Archive& archive, std::vector<T>& s,std::true_type) {
      uint32_t sz=0;
      archive.read(sz);
      s.resize(sz);
      archive.readBytes(s.data(),sz*sizeof(T));
      }

    // c-arrays
    template<class Archive, class T, size_t sz>
    static void writeValue(Archive& archive, const T (&s)[sz]) {
      implWriteArr(archive,s,std::is_trivial<T>());
      }

    template<class Archive, class T, size_t sz>
    static void implWriteArr(Archive& archive, const T (&s)[sz],std::false_type) {
      for(size_t i=0; i<sz; ++i)
        archive.write(s[i]);
      }

    template<class Archive, class T, size_t sz>
    static void implWriteArr(Archive& archive, const T (&s)[sz],std::true_type) {
      archive.writeBytes(s,sz*sizeof(T));
      }

    template<class Archive, class T, size_t sz>
    static void readValue(Archive& archive, T (&s)[sz]) {
      implReadArr(archive,s,std::is_trivial<T>());
      }

    template<class Archive, class T, size_t sz>
    static void implReadArr(Archive& archive, T (&s)[sz],std::false_type) {
      for(size_t i=0; i<sz; ++i)
        archive.read(s[i]);
      }

    template<class Archive, class T, size_t sz>
    static void implReadArr(Archive& archive, T (&s)[sz],std::true_type) {
      archive.readBytes(s,sz*sizeof(T));
      }

    void implWrite(const Tempest::Pixmap& p);
    void implWrite(const Tempest::Pixmap& p, const char* ext);
    template<class Ext>
    void implWrite(std::tuple<const Tempest::Pixmap&, Ext> p) { implWrite(std::get<0>(p), std::get<1>(p)); }
    void implRead (Tempest::Pixmap&       p);

  private:
    static size_t writeFunc(void *pOpaque, uint64_t file_ofs, const void *pBuf, size_t n);
    static size_t readFunc (void *pOpaque, uint64_t file_ofs, void *pBuf, size_t n);

    void   closeEntry();
    bool   implSetEntry(std::string_view e);
    uint32_t implDirectorySize(std::string_view e);

    uint16_t                 curVer = Version::Current;
    uint16_t                 wldVer = Version::Current;

    std::unordered_set<std::string> outFileList;

    mz_zip_archive           impl      = {};
    std::string              entryName;
    std::vector<uint8_t>     entryBuf;
    uint64_t                 curOffset = 0;
    uint64_t                 readOffset = 0;
    Tempest::ODevice*        fout      = nullptr;
    Tempest::IDevice*        fin       = nullptr;
  };
