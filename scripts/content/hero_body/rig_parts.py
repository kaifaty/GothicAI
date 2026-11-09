"""The render rig's extra parts on MPFB's game_engine rig, one table for the export, the writer and the offline bench
(reports/engine/2026-11-modern-skeleton, «Ноги» and «Верх тела»). Plain data, no imports.

- KNEES, SOFT: the knees bend as Gothic's (render-rigs.json root.knees; it replaced the pelvis lift of «Ноги») and the
  legs' soft IK (legs[].soft).
- TWISTS: twist bones. name, parent, of, against, axis, t, share: a bone under `parent` at `t` of the way to `axis`
  turns about the parent's axis by share x the twist of `of` against `against` (render-rigs.json twists). The upper
  arm's and the thigh's own twist is taken off near the shoulder and the hip (share < 0); the hand's twist is put on
  along the forearm (share > 0). Gothic's clips twist the hand against the forearm by up to 120 degrees (the bow holds
  117), the upper arm against the clavicle by up to 134 and the thigh against the pelvis by up to 61.
- TWIST_NODES: how a parent's skin weight spreads along it over itself and its twist bones: (t, bone), linear between.
- SHOULDER: the scapulohumeral rhythm - the clavicle turns further towards the arm by SHARE x (rise - FROM), at most
  MAX degrees (render-rigs.json shoulders).
- arms(): the arms' IK (render-rigs.json arms): the left hand holds on to the right one (a 2H hilt, a bow) and both go
  onto Gothic's at a mob, as the game says; ARM_SOFT cm of soft reach («Аудит»).
"""

KNEES, SOFT = True, 0.5
ARM_SOFT = 1.0

TWISTS = [  # name, parent, of, against, axis, t, share
    ('upperarm_twist_01_{s}', 'upperarm_{s}', 'upperarm_{s}', 'clavicle_{s}', 'lowerarm_{s}', 0.0, -1.0),
    ('upperarm_twist_02_{s}', 'upperarm_{s}', 'upperarm_{s}', 'clavicle_{s}', 'lowerarm_{s}', 0.5, -0.5),
    ('lowerarm_twist_02_{s}', 'lowerarm_{s}', 'hand_{s}', 'lowerarm_{s}', 'hand_{s}', 0.5, 0.5),
    ('lowerarm_twist_01_{s}', 'lowerarm_{s}', 'hand_{s}', 'lowerarm_{s}', 'hand_{s}', 1.0, 1.0),
    ('thigh_twist_01_{s}', 'thigh_{s}', 'thigh_{s}', 'pelvis', 'calf_{s}', 0.0, -1.0),
    ('thigh_twist_02_{s}', 'thigh_{s}', 'thigh_{s}', 'pelvis', 'calf_{s}', 0.5, -0.5),
]
TWIST_NODES = {'upperarm_{s}': [(0.0, 'upperarm_twist_01_{s}'), (0.5, 'upperarm_twist_02_{s}'), (1.0, 'upperarm_{s}')],
               'lowerarm_{s}': [(0.0, 'lowerarm_{s}'), (0.5, 'lowerarm_twist_02_{s}'), (1.0, 'lowerarm_twist_01_{s}')],
               'thigh_{s}': [(0.0, 'thigh_twist_01_{s}'), (0.5, 'thigh_twist_02_{s}'), (1.0, 'thigh_{s}')]}
TWIST_AXIS = {'upperarm_{s}': 'lowerarm_{s}', 'lowerarm_{s}': 'hand_{s}', 'thigh_{s}': 'calf_{s}'}

SHOULDER = {'share': 0.33, 'from': 45.0, 'max': 30.0}
SIDES = ('l', 'r')


def twists(sd):
    """TWISTS of one side with the names filled in."""
    return [tuple(x.format(s=sd) if isinstance(x, str) else x for x in row) for row in TWISTS]


def arms():
    """The arms entries of render-rigs.json, the held one after the one it holds."""
    return [{'upper': f'upperarm_{sd}', 'lower': f'lowerarm_{sd}', 'hand': f'hand_{sd}', 'source_hand': f'BIP01 {sd.upper()} HAND',
             'soft': ARM_SOFT, **({'holds': 'hand_r'} if sd == 'l' else {})} for sd in ('r', 'l')]


def table():
    """The twists and shoulders entries of render-rigs.json."""
    tw = [{'bone': n, 'of': of, 'against': ag, 'axis': ax, 'share': sh}
          for sd in SIDES for n, _p, of, ag, ax, _t, sh in twists(sd)]
    sh = [{'clavicle': f'clavicle_{sd}', 'arm': f'upperarm_{sd}', 'elbow': f'lowerarm_{sd}', 'chest': 'spine_03',
           **SHOULDER} for sd in SIDES]
    return tw, sh
