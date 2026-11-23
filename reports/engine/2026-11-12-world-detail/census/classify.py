import re
def strip(name):
    n=name.upper()
    if n.startswith('S:'):
        n=n[2:]
        i=n.find('_')
        # S:<ROOM>_<MATERIAL>; rooms like TROLLCAVE04, GRPCAVE05, XARCAVE01
        if i>0 and re.match(r'^[A-Z]+\d+',n[:i]) or (i>0 and n[:i].endswith(('CAVE','HAUS'))):
            n=n[i+1:]
    return n
VEG=('TREE','BRANCH','BUSH','PLANT','LEAVE','BARK','DUCKWEED','SEEROSE','GRASS_DECAL','FULLALPHA','FERN','FARN','IVY','LEAF','REED','SCHILF','VINE','LIANE','WEED','ALPHA','DECAL','MUSHROOM','ROPE','SPIDER','WEB')
WATER=('WATER','STREAM_01','OWODWFALL','WASSER')
GROUND=('GRASS','WALDBODEN','SAND','KIES','PATH','ACKER','BODEN_','_BODEN','WBODEN','STONEGROUND','SEESTRAND','SEEBODEN','SWEETGRASS','WASTELAND','STREAM','GROUND','DIRT','LEHM','ERDE','FIELD','MEADOW','MOOR','SNOW','SCHNEE')
ROCK=('STEIN','FELS','MOUNTAIN','CAVEWALL','SURROUNDING','OMWA','ROCK','LAVA','CLIFF','STONE_BIG','OW_SURFACE','VOLCANO','GOLEM','CRYSTAL')
def classify(name,group):
    n=strip(name)
    if group==5 or any(k in n for k in ('WATER','OWODWFALL')): return 'water'
    if n.startswith(('OW_ICEDRAGON','OW_STONEDRAGON','OW_SWAMPDRAGON','OW_FIREDRAGON','OW_FOREST','OWODWA','OWODFL')) and 'FALL' not in n:
        if any(k in n for k in ('SNOW','GROUND','FORESTGROUND','STRAND','LAKE')) and 'GROUNDSTONES' not in n: return 'ground'
        if 'BRETTER' in n: return 'man-made'
        if 'FOREST_STONE' in n: return 'vegetation'
        return 'rock'
    if 'LOOKTHROUGH' in n: return 'water'
    if 'NW_SEQ_' in n:
        if any(k in n for k in ('TREEBARK','TREE')): return 'vegetation'
        if any(k in n for k in ('MAYA_WALL','MAYA_GROUND','MAYA_STONE_TRANS')) and 'NATURESTONE' not in n: return 'man-made'
        if any(k in n for k in ('CANYONWALL','MOUNTAIN','NATURESTONE','WALL_TRANS','WALLTRANS')): return 'rock'
        if any(k in n for k in ('WHITESAND','GROUND','GRASSPATH','SWAMPGROUND','NATURE_GRASS','PATH')): return 'ground'
        return 'man-made'
    if n.startswith('NW_NATURE') or n.startswith('OW_NATURE') or n.startswith('OW_SURFACE') or n.startswith('NW_MISC_FULLALPHA'):
        if any(k in n for k in VEG): 
            if 'GRASS_DECAL' in n or 'SWEETGRASS' in n: return 'ground' if 'SWEETGRASS' in n else 'vegetation'
            if 'DARKWOOD_TREE_QUAD' in n: return 'ground'
            return 'vegetation'
        if any(k in n for k in ROCK) and not any(k in n for k in ('PATH','BODEN','GROUND')): return 'rock'
        if any(k in n for k in GROUND): return 'ground'
        if 'STONE' in n: return 'rock'
        return 'other'
    if any(k in n for k in ('DUCKWEED','SEEROSE','FULLALPHA','IVY','ROPE','SPIDER','WEB','MUSHROOM','TREE','BUSH','LEAF','FERN')): return 'vegetation'
    if any(k in n for k in ('SURROUNDING','CAVEWALL','OW_SURFACE_MOUNTAIN','OMWA','VOLCANO','FELS')) : return 'rock'
    if 'WASTELAND' in n or 'OW_SURFACE' in n: return 'ground'
    if any(k in n for k in ('CITY','HARBOUR','MONASTERY','SHIP','MAYA','XARDAS','BAUERNHAUS','STONEWALL','ROOF','WALL','OLDSTONEPATH','FACHWERK','DUNGEON','BODENDIELEN','HOUSE','HAUS','TOWER','FENCE','BRIDGE','STAIR','WOOD','FLOOR','CASTLE','KLOSTER','OC_','OM_','OW_MISC','NW_MISC','NW_SEQ')): return 'man-made'
    if group in (1,3): return 'man-made'
    return 'other'
