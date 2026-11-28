#pragma once

#include <compare>
#include <cstdint>
#include <functional>

class SaveArchive;

namespace modkit {

enum class EntityKind : uint8_t {
  None        = 0,
  Npc         = 1,
  Interactive = 2,
  WorldItem   = 3,
  };

// Stable 64-bit persistent identifier for a world entity (Npc, Interactive, WorldItem).
// Zero denotes an unassigned/null handle.
struct EntityId final {
  uint64_t raw = 0;

  constexpr bool valid() const noexcept { return raw != 0; }
  constexpr explicit operator bool() const noexcept { return raw != 0; }
  constexpr auto operator<=>(const EntityId&) const noexcept = default;
  };

// Stable 64-bit persistent identifier for an item stack/instance (inventory or world).
// Zero denotes an unassigned/null handle.
struct ItemUid final {
  uint64_t raw = 0;

  constexpr bool valid() const noexcept { return raw != 0; }
  constexpr explicit operator bool() const noexcept { return raw != 0; }
  constexpr auto operator<=>(const ItemUid&) const noexcept = default;
  };

// Monotonic session-owned ID generator persisted in v58 saves (`game/modkit-ids`).
class IdGenerator final {
  public:
    static constexpr uint32_t SaveMagic   = 0x3144494Du; // "MID1"
    static constexpr uint16_t SaveVersion = 1;

    EntityId allocEntity() noexcept { return EntityId{nextEntity++}; }
    ItemUid  allocItem()   noexcept { return ItemUid{nextItem++}; }

    void observeEntity(EntityId id) noexcept {
      if(id.raw >= nextEntity)
        nextEntity = id.raw + 1;
      }

    void observeItem(ItemUid uid) noexcept {
      if(uid.raw >= nextItem)
        nextItem = uid.raw + 1;
      }

    uint64_t peekNextEntity() const noexcept { return nextEntity; }
    uint64_t peekNextItem()   const noexcept { return nextItem; }

    void save(SaveArchive& fout) const;
    bool load(SaveArchive& fin);

  private:
    uint64_t nextEntity = 1;
    uint64_t nextItem   = 1;
  };

} // namespace modkit

namespace std {

template<>
struct hash<modkit::EntityId> {
  size_t operator()(const modkit::EntityId& id) const noexcept {
    return std::hash<uint64_t>{}(id.raw);
    }
  };

template<>
struct hash<modkit::ItemUid> {
  size_t operator()(const modkit::ItemUid& id) const noexcept {
    return std::hash<uint64_t>{}(id.raw);
    }
  };

} // namespace std
