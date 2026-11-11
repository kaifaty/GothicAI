# The generator's model in the head's frame, for the scripts of this folder.
#   OPT = ref_model.options(sys.argv); rv, rc = ref_model.load(raw, params, eye, OPT)
# The model is put as reg_export.py puts it. Options, removed from argv:
#   --place place.json   (place_ref.py) the model is scaled about the eye point and shifted, so it lies on the
#                        concept and on the head that was registered to a placed model;
#   --tone tone.json     (tone_ref.py) the model's colour is brought to the concept's: per channel gain*c^gamma.
# Returns the points in metres and the colour as sRGB 0..255. raw_colour() is the colour before --tone: the tests
# that tell hair, skin, cloth and eyeballs apart were measured on it.
import json
import numpy as np


def take(argv, name):
    """Removes `name value` from argv and returns the value, or None."""
    if name not in argv: return None
    i = argv.index(name); val = argv[i+1]; del argv[i:i+2]
    return val


def options(argv):
    opt = {}
    for key in ('place', 'tone'):
        path = take(argv, '--'+key)
        opt[key] = json.load(open(path)) if path else None
    return opt


def load(raw, P, eye, opt):
    unit = 887/1.1*0.0615/P['ipd_px']; R = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], np.float32)
    rv = (raw['vertices']@R.T)*unit+np.array(P['ref_origin'], np.float32)
    if opt.get('place'):
        rv = (rv-eye)*opt['place']['scale']+eye+np.array(opt['place']['shift_mm'])/1000
    rc = np.clip(raw['color'], 0, 1)**(1/2.2)
    if opt.get('tone'):
        rc = np.array(opt['tone']['gain'])*rc**np.array(opt['tone']['gamma'])
    return rv.astype(np.float32), np.clip(rc, 0, 1)*255


def raw_colour(raw):
    return np.clip(raw['color'], 0, 1)**(1/2.2)*255
