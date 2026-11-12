"""Class rules of the world census (world_census.py): material classes of the world mesh and object classes of
the vobs, as tables of name patterns.

Classes come from names, not from an authoritative tag: Gothic's material `group` (2 stone, 3 wood, 4 earth,
5 water) is used for water and as the last resort for man-made. The rules were tuned on NEWWORLD until class
`other` held about 0.2 % of its triangles (the report states the share); OLDWORLD (`OW_*`) is roughly right, ADDONWORLD (`NW_SEQ_*`) is rough. The
rules and the numbers they give are those of the research census
(reports/engine/2026-11-12-world-detail/research/world_census.md, "How to read the numbers").

A rule matches when every condition it has holds; the first matching rule of a table gives the class:
- prefix: the name starts with one of these strings;
- all: a list of regular expressions, each must be found in the name;
- none: a regular expression that must not be found;
- group: the material's Gothic group is one of these.
"""
from __future__ import annotations

import re
from dataclasses import dataclass

MATERIAL_CLASSES = ('ground', 'rock', 'water', 'man-made', 'vegetation', 'other')
NATURAL = ('ground', 'rock')
OBJECT_CLASSES = ('trees', 'trunks/logs', 'bushes', 'plants/grass/ferns', 'rocks', 'other static visuals')


def words(*parts: str) -> str:
    """A regular expression that finds any of these plain substrings."""
    return '|'.join(re.escape(p) for p in parts)


@dataclass(frozen=True)
class Rule:
    cls: str
    prefix: tuple[str, ...] = ()
    all: tuple[str, ...] = ()
    none: str = ''
    group: tuple[int, ...] = ()

    def __post_init__(self):
        object.__setattr__(self, '_all', tuple(re.compile(p) for p in self.all))
        object.__setattr__(self, '_none', re.compile(self.none) if self.none else None)

    def matches(self, name: str, group: int | None = None) -> bool:
        if self.prefix and not name.startswith(self.prefix):
            return False
        if self.group and group not in self.group:
            return False
        if self._none is not None and self._none.search(name):
            return False
        return all(p.search(name) for p in self._all)


def first(rules: tuple[Rule, ...], name: str, group: int | None = None, default: str = 'other') -> str:
    for rule in rules:
        if rule.matches(name, group):
            return rule.cls
    return default


# --- materials of the world mesh

# `S:<ROOM>_<MATERIAL>` is a per-room copy of a material: the room part is stripped when it is a word with digits
# (TROLLCAVE04, GRPCAVE05) or ends with CAVE or HAUS.
ROOM_COPY = re.compile(r'^S:(?:([A-Z]+\d+[^_]*)|([^_]*(?:CAVE|HAUS)))_')
# A room material of a cave, for the "where" of multi-layer cells.
CAVE_ROOM = re.compile(r'^S:.*CAVE')

VEG = words('TREE', 'BRANCH', 'BUSH', 'PLANT', 'LEAVE', 'BARK', 'DUCKWEED', 'SEEROSE', 'GRASS_DECAL', 'FULLALPHA',
            'FERN', 'FARN', 'IVY', 'LEAF', 'REED', 'SCHILF', 'VINE', 'LIANE', 'WEED', 'ALPHA', 'DECAL', 'MUSHROOM',
            'ROPE', 'SPIDER', 'WEB')
GROUND = words('GRASS', 'WALDBODEN', 'SAND', 'KIES', 'PATH', 'ACKER', 'BODEN_', '_BODEN', 'WBODEN', 'STONEGROUND',
               'SEESTRAND', 'SEEBODEN', 'SWEETGRASS', 'WASTELAND', 'STREAM', 'GROUND', 'DIRT', 'LEHM', 'ERDE',
               'FIELD', 'MEADOW', 'MOOR', 'SNOW', 'SCHNEE')
ROCK = words('STEIN', 'FELS', 'MOUNTAIN', 'CAVEWALL', 'SURROUNDING', 'OMWA', 'ROCK', 'LAVA', 'CLIFF', 'STONE_BIG',
             'OW_SURFACE', 'VOLCANO', 'GOLEM', 'CRYSTAL')
MAN_MADE = words('CITY', 'HARBOUR', 'MONASTERY', 'SHIP', 'MAYA', 'XARDAS', 'BAUERNHAUS', 'STONEWALL', 'ROOF', 'WALL',
                 'OLDSTONEPATH', 'FACHWERK', 'DUNGEON', 'BODENDIELEN', 'HOUSE', 'HAUS', 'TOWER', 'FENCE', 'BRIDGE',
                 'STAIR', 'WOOD', 'FLOOR', 'CASTLE', 'KLOSTER', 'OC_', 'OM_', 'OW_MISC', 'NW_MISC', 'NW_SEQ')

# OLDWORLD dragon areas and forests: one family of names with its own words.
OW_AREAS = ('OW_ICEDRAGON', 'OW_STONEDRAGON', 'OW_SWAMPDRAGON', 'OW_FIREDRAGON', 'OW_FOREST', 'OWODWA', 'OWODFL')
NATURE = ('NW_NATURE', 'OW_NATURE', 'OW_SURFACE', 'NW_MISC_FULLALPHA')
SEQ = words('NW_SEQ_')

MATERIAL_RULES: tuple[Rule, ...] = (
    Rule('water', group=(5,)),
    Rule('water', all=(words('WATER', 'OWODWFALL'),)),
    # OLDWORLD areas (waterfalls excluded)
    Rule('ground', prefix=OW_AREAS, all=(words('SNOW', 'GROUND', 'FORESTGROUND', 'STRAND', 'LAKE'),),
         none=words('FALL', 'GROUNDSTONES')),
    Rule('man-made', prefix=OW_AREAS, all=(words('BRETTER'),), none=words('FALL')),
    Rule('vegetation', prefix=OW_AREAS, all=(words('FOREST_STONE'),), none=words('FALL')),
    Rule('rock', prefix=OW_AREAS, none=words('FALL')),
    Rule('water', all=(words('LOOKTHROUGH'),)),
    # ADDONWORLD sequences
    Rule('vegetation', all=(SEQ, words('TREEBARK', 'TREE'))),
    Rule('man-made', all=(SEQ, words('MAYA_WALL', 'MAYA_GROUND', 'MAYA_STONE_TRANS')), none=words('NATURESTONE')),
    Rule('rock', all=(SEQ, words('CANYONWALL', 'MOUNTAIN', 'NATURESTONE', 'WALL_TRANS', 'WALLTRANS'))),
    Rule('ground', all=(SEQ, words('WHITESAND', 'GROUND', 'GRASSPATH', 'SWAMPGROUND', 'NATURE_GRASS', 'PATH'))),
    Rule('man-made', all=(SEQ,)),
    # nature materials of both islands
    Rule('ground', prefix=NATURE, all=(VEG, words('SWEETGRASS'))),
    Rule('vegetation', prefix=NATURE, all=(VEG, words('GRASS_DECAL'))),
    Rule('ground', prefix=NATURE, all=(VEG, words('DARKWOOD_TREE_QUAD'))),
    Rule('vegetation', prefix=NATURE, all=(VEG,)),
    Rule('rock', prefix=NATURE, all=(ROCK,), none=words('PATH', 'BODEN', 'GROUND')),
    Rule('ground', prefix=NATURE, all=(GROUND,)),
    Rule('rock', prefix=NATURE, all=(words('STONE'),)),
    Rule('other', prefix=NATURE),
    # everything else
    Rule('vegetation', all=(words('DUCKWEED', 'SEEROSE', 'FULLALPHA', 'IVY', 'ROPE', 'SPIDER', 'WEB', 'MUSHROOM',
                                  'TREE', 'BUSH', 'LEAF', 'FERN'),)),
    Rule('rock', all=(words('SURROUNDING', 'CAVEWALL', 'OW_SURFACE_MOUNTAIN', 'OMWA', 'VOLCANO', 'FELS'),)),
    Rule('ground', all=(words('WASTELAND', 'OW_SURFACE'),)),
    Rule('man-made', all=(MAN_MADE,)),
    Rule('man-made', group=(1, 3)),
)


def strip_room(name: str) -> str:
    """The material name without the room part of an `S:<ROOM>_` copy, upper case."""
    n = name.upper()
    if n.startswith('S:'):
        m = ROOM_COPY.match(n)
        n = n[m.end():] if m else n[2:]
    return n


def material_class(name: str, group: int | None) -> str:
    return first(MATERIAL_RULES, strip_room(name), group)


def is_cave_room(name: str) -> bool:
    return bool(CAVE_ROOM.match(name))


# --- objects (vobs)

# Visual names that are not meshes: textures (decals), particle effects, sounds; the name without extension.
NOT_MESH = re.compile(r'(\.TGA|\.PFX|\.WAV|\.SLV|\.)$|^$')
MESH_EXTENSION = re.compile(r'\.(3DS|MDL|MDS|ASC|MMS|MMB)$')

OBJECT_RULES: tuple[Rule, ...] = (
    Rule('trees', all=(r'TREE|TANNE|BAUM(?!STAMM)|PINE|PALM',), none=words('TREEBARK')),
    Rule('trunks/logs', all=(words('BAUMSTAMM', 'STUMP', 'WOODLOG'),)),
    Rule('bushes', all=(words('BUSH', 'HECKE', 'LONG_BUSH'),)),
    Rule('plants/grass/ferns', all=(words('FARN', 'FERN', 'GRASS', 'PLANT', 'REED', 'SCHILF', 'FLOWER', 'HERB',
                                          'MUSHROOM', 'WEED', 'MOSS', 'HANGPLANT', 'CACTUS', 'VINE', 'LIANE'),)),
    Rule('plants/grass/ferns', prefix=('OW_NATURE_SIDE', 'NW_NATURE_SIDE')),
    Rule('rocks', all=(r'STONE_|STEIN|ROCK|FELS|BOULDER|COUPLESTONES|STONEBIG|_STONE$|ROCKS',),
         none=words('STONEWALL', 'STONEMILL', 'STONEHENGE', 'PFLASTER')),
)


def object_class(visual: str) -> str | None:
    """Class of a vob by its visual name; None for a visual that is not a mesh or model."""
    n = visual.upper()
    if NOT_MESH.search(n):
        return None
    return first(OBJECT_RULES, MESH_EXTENSION.sub('', n), default='other static visuals')
