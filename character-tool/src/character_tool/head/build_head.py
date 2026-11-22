"""The head of an NPC from its concept folder in one command (docs/npc-pipeline.md, stage 5;
reports/content/2026-11-npc-pipeline/build.md).

  ~/.local/share/gothic-ai/mpfb/venv/bin/python head/build_head.py <concept dir> \
      [--content <content root>] [--work <dir>] [--from <step>] [--only <step>] [--list]
      [--gpu-authorization <label>] [--mesh-runner <path to run-mesh3d.sh>]

<concept dir>/npc.json:
  {"name": "hero-3a",                   the work folder's name
   "instance": "PC_HERO",               the script instance whose head this is (only the hero for now)
   "sheet": "sheet.png",                front | left profile side by side, square halves (image_gen)
   "bald": "bald.png",                  the same sheet bald, shaven, neutral (image_gen)
   "params": "params.json",             optional: the MPFB start and the concept's frame by hand (fit/params-*.json);
                                        without it auto_params.py finds the frame, and "macro", "race", "targets"
                                        (fit_head.py) may be given here
   "picks": "picks.json",               optional: ears and the profile midline by hand (fit/reg/picks-*.json);
                                        without it the ears follow the dense model as plain surface
   "mesh": {"full": "raw.npz", "bald": "raw.npz"},   optional: the generator's dense models (run-mesh3d.sh);
                                        without it they come from the cache in work/build/_mesh by the hash of the
                                        views, or Pixal3D makes them with --gpu-authorization <label>
   "shapes": "mark"}                    optional: the face shapes of Audio2Face's Mark instead of MPFB's and the face
                                        profile re-scaled for them (step "shapes", face-shapes.md "Формы Mark в
                                        игре"); without it the head keeps MPFB's shapes
Relative paths are relative to the concept dir.

GOTHIC_AI_DATA holds the mpfb environments, Blender resources, template inputs, detector, Mark and texpack.
BLENDER, BLENDER_USER_RESOURCES, MPFB_PYTHON, MEDIAPIPE_PYTHON, FACE_LANDMARKER and TEXPACK can override
their individual paths. OPENGOTHIC_BASE_CACHE contains the imported game bases.
Uncached Pixal3D uses the packaged mesh3d runner with --gpu-authorization. To use another installed runner,
pass its path with --mesh-runner. The remote generator and its weights must already be available.

Every step writes into its own folder of the work dir (default ~/.local/share/gothic-ai/mpfb/work/build/<name>) and
leaves there a stamp: the hash of its commands, of the scripts they run and of its input files (size and time for
files over 4 MB). A step whose stamp matches and whose outputs exist is skipped, so a second run only redoes what
changed. A failing step stops the build with its name, the command and the tail of its log (<step>/log.txt).

Writes into <content root> (default <work>/content): characters/<folder>/head.glb, head.face.json, head.frame.json,
head.groom, materials/ and characters/characters.json (the instance's entry is replaced, others are kept). The sheet
of likeness is <work>/sheet/likeness.jpg, the measures <work>/measures.json, the sheet and the numbers of the shape
(outlines, hair and beard against the concept, shape_report.py) <work>/shape/shape.jpg and shape.json, the bald
head's outline against the concept's before and after the last pass <work>/outline/outline.jpg and outline.json,
the bald model over the head before and after its folds went into the vertices <work>/relief/relief.jpg and
relief.json, the lid lines read from the concept <work>/reg/eyes.jpg and eyes.json.
"""
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HEAD = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HEAD)
from head_paths import (BLENDER_RESOURCES, DATA_ROOT, FACE_LANDMARKER, MARK, MPFB, MPFB_DATA,
                        TEXPACK, base_cache, base_cache_root, executable)

F, FR, SK = os.path.join(HEAD, 'fit'), os.path.join(HEAD, 'fit/reg'), os.path.join(HEAD, 'skin')
PY = executable('MPFB_PYTHON', os.path.join(MPFB, 'venv/bin/python'))
MPY = executable('MEDIAPIPE_PYTHON', os.path.join(MPFB, 'venv-mp/bin/python'))
BL = executable('BLENDER', shutil.which('blender') or '/Applications/Blender.app/Contents/MacOS/Blender')
# template assets that are the same for every head
T = {'probe': os.path.join(MPFB, 'work/probe.blend'),
     'eye': os.path.join(MPFB, 'work/skin3a/eye_dark.png'),
     'base_skin': os.path.join(MPFB_DATA, 'skins/middleage_caucasian_male/middleage_lightskinned_male_diffuse.png'),
     'sched': os.path.join(FR, 'sched9.json'), 'lm': os.path.join(FR, 'lm.json'),
     'lid_chains': os.path.join(FR, 'lid_chains.json'), 'lm_fixed': os.path.join(FR, 'lm_fixed.json'),
     'points': os.path.join(HEAD, 'template_points.json'),
     'transforms': os.path.join(F, 'pixal3d-views-transforms.json')}
LID_BACK = '5.8'          # mm: lid depth from the template lid moved back (lm_pick.py, auto_targets.py)
BIG = 4 << 20


class Step:
    def __init__(self, name, cmds, inputs, outputs, note='', *, input_root=None):
        self.name, self.cmds, self.inputs, self.outputs, self.note = name, cmds, inputs, outputs, note
        self.input_root = input_root  # fingerprint the location of a shared cache or Blender profile


def file_key(p, h):
    if os.path.isdir(p):
        for root, _, files in sorted(os.walk(p)):
            for f in sorted(files): file_key(os.path.join(root, f), h)
        return
    st = os.stat(p)
    h.update(p.encode())
    if st.st_size > BIG: h.update(b'%d %d' % (st.st_size, st.st_mtime_ns))
    else: h.update(open(p, 'rb').read())


def stamp(step):
    h = hashlib.sha256()
    if step.input_root is not None:
        h.update(json.dumps(step.input_root).encode())
    for c in step.cmds:
        h.update(json.dumps(c if isinstance(c, list) else c.__name__).encode())
        if isinstance(c, list):
            for a in c:
                if isinstance(a, str) and a.endswith('.py') and os.path.isfile(a): file_key(a, h)
    for p in step.inputs:
        if not os.path.exists(p): raise SystemExit(f'build_head: step {step.name}: input missing: {p}')
        file_key(p, h)
    return h.hexdigest()


def run(step, work):
    d = os.path.join(work, step.name); os.makedirs(d, exist_ok=True)
    key = stamp(step); sp = os.path.join(d, '.stamp')
    if os.path.exists(sp) and open(sp).read() == key and all(os.path.exists(o) for o in step.outputs):
        print(f'= {step.name}: up to date'); return False
    if os.path.exists(sp): os.remove(sp)
    print(f'> {step.name} {step.note}', flush=True); t0 = time.time()
    env = dict(os.environ, GOTHIC_AI_DATA=DATA_ROOT, BLENDER_USER_RESOURCES=BLENDER_RESOURCES,
               FACE_LANDMARKER=FACE_LANDMARKER, TEXPACK=TEXPACK,
               OPENGOTHIC_BASE_CACHE=base_cache_root())
    with open(os.path.join(d, 'log.txt'), 'w') as log:
        for c in step.cmds:
            if callable(c):
                log.write(f'$ <{c.__name__}>\n'); log.flush()
                try: c()
                except Exception as e: fail(step, c.__name__, d, repr(e))
                continue
            log.write('$ ' + ' '.join(c) + '\n'); log.flush()
            r = subprocess.run(c, cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT)
            if r.returncode != 0: fail(step, ' '.join(c), d, f'exit {r.returncode}')
    missing = [o for o in step.outputs if not os.path.exists(o)]
    if missing: fail(step, 'outputs', d, 'not written: ' + ', '.join(missing))
    open(sp, 'w').write(key)
    print(f'  {step.name}: {time.time()-t0:.0f} s', flush=True); return True


def fail(step, what, d, why):
    tail = open(os.path.join(d, 'log.txt'), errors='replace').read().splitlines()[-25:]
    print(f'\nbuild_head: step "{step.name}" failed ({why})\n  {what}\n  log: {d}/log.txt\n' + '\n'.join(tail),
          file=sys.stderr)
    raise SystemExit(1)


def steps(cdir, cfg, work, content, gpu=None, mesh_runner=None):
    j = lambda *p: os.path.join(work, *p)
    src = lambda k: os.path.join(cdir, cfg[k])

    def fit_input(key, preset):
        # The original hero-3a recipe names these two checked-in presets by absolute paths to an old worktree.
        path = src(key)
        legacy = '/reports/content/2026-11-hero-head/' + preset
        if os.path.isabs(cfg[key]) and path.endswith(legacy) and not os.path.isfile(path):
            return os.path.join(HEAD, preset)
        return path

    blend = lambda f, script, *a: [BL, '-b', f, '-P', script, '--', *a]
    blank = lambda script, *a: [BL, '-b', '-P', script, '--', *a]
    npc = os.path.join(cdir, 'npc.json')
    params = fit_input('params', 'fit/params-3a.json') if 'params' in cfg else j('params.json')
    picks = fit_input('picks', 'fit/reg/picks-3a.json') if 'picks' in cfg else None
    raw = {k: j('mesh', k, 'raw.npz') for k in ('full', 'bald')}
    place = ['--place', j('full', 'place.json'), '--tone', j('full', 'tone.json')]
    folder = 'hero'                                                    # export_test_head.py writes the hero only
    VIEW = os.path.join(HEAD, 'concept_view.py')                          # imported by place_ref.py and head_outline.py
    EYES = os.path.join(HEAD, 'eye_lines.py')                             # imported by auto_targets.py, concept_colours.py, head_measures.py
    head = os.path.join(content, 'characters', folder)
    out = []

    # 1. the sheets as two square views side by side (a half narrower than the sheet is high is padded with the
    #    background), crops for the frames, cut-outs for the generator
    from PIL import Image
    sizes = {Image.open(src(k)).size for k in ('sheet', 'bald')}
    if len(sizes) != 1: raise SystemExit(f'build_head: the full and the bald sheet differ in size: {sizes}')
    w, h = sizes.pop(); side = max(w // 2, h)

    def cut():
        import numpy as np
        for kind in ('full', 'bald'):
            im = Image.open(src('sheet' if kind == 'full' else 'bald')).convert('RGB')
            a = np.asarray(im); bg = tuple(int(c) for c in np.median(np.concatenate([a[0], a[-1], a[:, 0], a[:, -1]]), axis=0))
            sheet = Image.new('RGB', (2 * side, side), bg)
            for i, view in enumerate(('front', 'profile')):
                half = im.crop((i * (w // 2), 0, (i + 1) * (w // 2), h))
                sheet.paste(half, (i * side + (side - half.width) // 2, (side - h) // 2))
            sheet.save(j('views', f'{kind}_sheet.png'))
            for i, view in enumerate(('front', 'profile')):
                sheet.crop((i * side, 0, (i + 1) * side, side)).save(j('views', f'{kind}_{view}.png'))
    sheets = {k: j('views', f'{k}_sheet.png') for k in ('full', 'bald')}
    out.append(Step('views', [cut] + [
        [PY, os.path.join(F, 'cutout.py'), sheets[k], j('views', k, f'view0{i}_azim0{a}.png'), str(i * side), '0', str(side)]
        for k in ('full', 'bald') for i, a in ((0, '00'), (1, '90'))] + [
        ['cp', T['transforms'], j('views', k, 'transforms.json')] for k in ('full', 'bald')] + [
        [MPY, os.path.join(HEAD, 'skull_check.py'), sheets['full'], sheets['bald']],
        [MPY, os.path.join(HEAD, 'face_points.py'), j('views', 'points')] + [
            f'{k}_front=' + j('views', f'{k}_front.png') for k in ('full', 'bald')]],
        [src('sheet'), src('bald'), T['transforms'], FACE_LANDMARKER],
        [j('views', f'{k}_{v}.png') for k in ('full', 'bald') for v in ('front', 'profile')]
        + [j('views', 'points', f'{k}_front.json') for k in ('full', 'bald')]))

    # 2. the dense models: given in npc.json, or from the cache by the hash of the views, or generated by Pixal3D on
    #    dev-ubuntu (run-mesh3d.sh: it stops the game's models for a few minutes, so only with --gpu-authorization)
    mesh = cfg.get('mesh', {})
    mesh_cache = os.path.join(MPFB, 'work', 'build', '_mesh')

    def meshes():
        for kind, dst in raw.items():
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if os.path.lexists(dst): os.remove(dst)
            if kind in mesh:
                os.symlink(os.path.join(cdir, mesh[kind]), dst); continue
            h = hashlib.sha256()
            for f in sorted(os.listdir(j('views', kind))): h.update(open(j('views', kind, f), 'rb').read())
            cache = os.path.join(mesh_cache, 'pixal3d-seed1-' + h.hexdigest()[:16])
            if not os.path.exists(os.path.join(cache, 'raw.npz')):
                if not gpu:
                    raise RuntimeError(f'no dense model for the {kind} views; generating one stops the game models on '
                                       f'dev-ubuntu for a few minutes: run again with --gpu-authorization <label>')
                label = f'{gpu}-{kind}'
                runner = (os.path.abspath(os.path.expanduser(mesh_runner)) if mesh_runner else
                          os.path.join(os.path.dirname(HEAD), 'backends', 'mesh3d', 'run-mesh3d.sh'))
                r = subprocess.run([runner, '--model', 'pixal3d',
                                    '--views', j('views', kind), '--seed', '1', '--min-free-mib', '21000',
                                    '--max-downtime', '1500', '--authorization', label],
                                   cwd=j('mesh'), capture_output=True, text=True)
                open(j('mesh', f'{kind}-run.log'), 'w').write(r.stdout + r.stderr)
                runs = [l.split('Results:', 1)[1].strip() for l in r.stdout.splitlines() if l.startswith('Results:')]
                if r.returncode != 0 or not runs:
                    raise RuntimeError(f'run-mesh3d.sh exit {r.returncode}, see mesh/{kind}-run.log')
                os.makedirs(cache, exist_ok=True)
                for f in ('raw.npz', 'meta.json'):
                    subprocess.run(['scp', '-q', f'dev-ubuntu:{runs[0]}/pixal3d-1024/{f}', os.path.join(cache, f)], check=True)
            os.symlink(os.path.join(cache, 'raw.npz'), dst)
    out.append(Step('mesh', [meshes], [j('views', 'full'), j('views', 'bald')] +
                    [os.path.join(cdir, v) for v in mesh.values()], list(raw.values()), 'the dense models',
                    input_root=mesh_cache if raw.keys() - mesh.keys() else None))

    # 3. the MPFB start
    if 'params' in cfg:
        out.append(Step('base', [blank(os.path.join(F, 'fit_head.py'), params, j('base'))], [params],
                        [j('base', 'fit.blend')], input_root=BLENDER_RESOURCES))
    else:
        auto = [PY, os.path.join(HEAD, 'auto_params.py'), j('views'), j('views', 'points'), npc]
        out.append(Step('base', [auto + [j('base', 'params.json')], blank(os.path.join(F, 'fit_head.py'), j('base', 'params.json'), j('base')),
                                 auto + [params, j('base')]],
                        [npc, j('views', 'points'), j('views', 'bald_front.png'), j('views', 'bald_profile.png')],
                        [j('base', 'fit.blend'), params], 'the MPFB start and the frame of the concept',
                        input_root=BLENDER_RESOURCES))
    fit = j('base', 'fit.blend')

    # 4. registration onto the bald model
    R = j('reg')
    # place_ref.py keeps the model as exported in ref_unplaced.npz and starts from it when it is there: a rerun of the
    # step must not start from the model of the run before (another origin in params, the same file name)
    out.append(Step('reg', [
        ['rm', '-f', j('reg', 'ref_unplaced.npz')],
        blend(fit, os.path.join(FR, 'reg_export.py'), raw['bald'], params, R),
        blend(fit, os.path.join(FR, 'groups.py'), j('reg', 'groups.npz')),
        blend(fit, os.path.join(FR, 'eye_deltas.py'), j('reg', 'eye_deltas.npz')),
        blend(fit, os.path.join(FR, 'fu_deltas.py'), j('reg', 'eye_deltas.npz')),
        ['cp', T['lm'], T['lid_chains'], R],
        [PY, os.path.join(FR, 'concept_frame.py'), R, params, j('views', 'bald_front.png'), j('views', 'bald_profile.png')],
        [PY, os.path.join(HEAD, 'place_ref.py'), R, params, raw['bald'], j('views', 'bald_front.png'), j('views', 'bald_profile.png')]],
        [fit, raw['bald'], params, j('views', 'bald_front.png'), j('views', 'bald_profile.png'), T['lm'], T['lid_chains'], VIEW],
        [j('reg', n) for n in ('mpfb.npz', 'ref.npz', 'views.json', 'groups.npz', 'eye_deltas.npz', 'place.json')],
        'export, frames, placing the bald model', input_root=BLENDER_RESOURCES))
    targets = [PY, os.path.join(HEAD, 'auto_targets.py'), R, j('reg', 'lid_chains.json'), j('reg', 'points', 'concept_front.json'),
               j('reg', 'points', 'tmpl_front.json')]
    out.append(Step('targets', [
        [MPY, os.path.join(HEAD, 'face_points.py'), j('reg', 'points'), 'concept_front=' + j('reg', 'concept_front.png'),
         'tmpl_front=' + j('reg', 'lm_mpfb_front.png')]] + (
        [[PY, os.path.join(FR, 'lm_pick.py'), R, picks, T['lm_fixed'], LID_BACK],
         targets + [j('reg', 'lm_fixed.json'), j('reg', 'lm_fixed.json'), LID_BACK],
         # ears picked by hand stay; the hairstyle still needs to know which skin is an ear
         [PY, os.path.join(HEAD, 'ear_points.py'), R, j('reg', 'lm_fixed.json'), j('reg', 'lm_fixed.json'), j('reg', 'ears.jpg'),
          '--mask', j('reg', 'ears.npz'), '--keep-hand']] if picks else
        [targets + [T['lm_fixed'], j('reg', 'lm_fixed.json'), LID_BACK, '--no-hand'],
         # no hand picks: the rims of the ears are found on the model and the template (ears.jpg shows them)
         [PY, os.path.join(HEAD, 'ear_points.py'), R, j('reg', 'lm_fixed.json'), j('reg', 'lm_fixed.json'), j('reg', 'ears.jpg'),
          '--mask', j('reg', 'ears.npz')]]),
        [j('reg', 'ref.npz'), j('reg', 'concept_front.png'), j('reg', 'lm_mpfb_front.png'), T['lm_fixed'], EYES,
         FACE_LANDMARKER] + ([picks] if picks else []),
        [j('reg', 'lm_fixed.json'), j('reg', 'eyes.json')], 'face points, the lid lines of the concept, landmark targets'))
    out.append(Step('solve', [[PY, os.path.join(FR, 'reg_solve.py'), R, T['sched']]],
                    [j('reg', 'lm_fixed.json'), j('reg', 'ref.npz'), j('reg', 'mpfb.npz'), T['sched']],
                    [j('reg', 'reg.npz')], 'non-rigid registration'))
    # the registration lays the vertices on the bald model, and bridges every fold narrower than its faces. The model's
    # relief goes into the vertices (what is finer is kept for the normal map), then the head's outline is laid on the
    # concept's, where the model itself departs from the concept
    out.append(Step('relief', [[PY, os.path.join(HEAD, 'head_relief.py'), R, j('relief'), '--ears', j('reg', 'ears.npz')]],
                    [j('reg', n) for n in ('reg.npz', 'ref.npz', 'mpfb.npz', 'groups.npz', 'ears.npz')],
                    [j('relief', n) for n in ('reg.npz', 'samples.npz', 'relief.json')], "the model's folds into the vertices"))
    out.append(Step('outline', [[PY, os.path.join(HEAD, 'head_outline.py'), R, params, j('views', 'bald_front.png'),
                                 j('views', 'bald_profile.png'), j('outline'), '--ears', j('reg', 'ears.npz'),
                                 '--reg', j('relief', 'reg.npz')]],
                    [j('relief', 'reg.npz')] + [j('reg', n) for n in ('reg_log.json', 'ref.npz', 'mpfb.npz', 'groups.npz', 'ears.npz')]
                    + [params, j('views', 'bald_front.png'), j('views', 'bald_profile.png'), VIEW],
                    [j('outline', 'reg.npz'), j('outline', 'outline.json')], 'faces onto the model, the outline onto the concept'))
    C1 = j('round')
    out.append(Step('round', [
        ['cp', j('outline', 'reg.npz'), j('reg', 'reg_log.json'), j('reg', 'lm_fixed.json'), C1],
        blend(fit, os.path.join(FR, 'reg_apply.py'), j('outline', 'reg.npz'), j('reg', 'mpfb.npz'), j('round', 'reg.blend')),
        blend(j('round', 'reg.blend'), os.path.join(FR, 'subsurf.py')),
        [PY, os.path.join(FR, 'blink_build.py'), R, C1, j('round', 'blink.npz')]],
        [j('outline', 'reg.npz'), fit], [j('round', n) for n in ('reg.blend', 'reg_sub.blend', 'blink.npz')],
        'the head, its subdivided copy, the blink'))

    # 5. the full model on the concept, brought to its tone
    out.append(Step('full', [
        ['rm', '-f', j('full', 'ref_unplaced.npz')],
        blend(fit, os.path.join(FR, 'reg_export.py'), raw['full'], params, j('full')),
        [PY, os.path.join(FR, 'concept_frame.py'), j('full'), params, j('views', 'full_front.png'), j('views', 'full_profile.png')],
        [PY, os.path.join(HEAD, 'place_ref.py'), j('full'), params, raw['full'], j('views', 'full_front.png'), j('views', 'full_profile.png')],
        # outlines do not hold the full model along the view, and its outline is mostly hair: where both are bare
        # skin, it is put onto the head
        [PY, os.path.join(HEAD, 'place_face.py'), j('full'), params, raw['full'], R, j('round', 'reg.npz'), j('views')],
        [PY, os.path.join(SK, 'tone_ref.py'), j('full', 'views.json'), params, raw['full'], j('views', 'full_front.png'),
         j('full', 'tone.json'), '--place', j('full', 'place.json')],
        [MPY, os.path.join(HEAD, 'face_points.py'), j('full', 'points'), 'concept_front=' + j('full', 'concept_front.png')]],
        [fit, raw['full'], params, j('views', 'full_front.png'), j('views', 'full_profile.png'), j('round', 'reg.npz'),
         j('reg', 'mpfb.npz'), j('reg', 'groups.npz'), VIEW, FACE_LANDMARKER],
        [j('full', n) for n in ('place.json', 'tone.json', 'concept_front.png', 'points/concept_front.json')],
        'placing the full model, its tone'))

    # 6. skin: colour from the full model, brows from the concept
    U, GR, V = j('skin', 'uv.npz'), j('reg', 'groups.npz'), j('reg', 'views.json')
    out.append(Step('skin', [
        blend(j('round', 'reg.blend'), os.path.join(SK, 'dump_uv.py'), U),
        # the beard is where the full model stands over the skin of the lower face: no colour thresholds
        [PY, os.path.join(HEAD, 'hair', 'beard_layer.py'), U, GR, V, params, raw['full'], j('skin'), *place],
        [PY, os.path.join(SK, 'bake_ref.py'), U, GR, V, params, raw['full'], T['base_skin'], j('skin'), '4096', *place,
         '--offsets', j('skin', 'beard.npz'), '--front', j('views', 'full_front.png'), j('reg', 'lid_chains.json')],
        [PY, os.path.join(SK, 'brow_paint.py'), U, GR, V, j('full', 'concept_front.png'),
         j('full', 'points', 'concept_front.json'), j('skin', 'skin.png'), j('skin', 'skin_brows.png')]],
        [j('round', 'reg.blend'), GR, V, params, raw['full'], j('full', 'place.json'), j('full', 'tone.json'),
         j('full', 'concept_front.png'), j('full', 'points', 'concept_front.json'), T['base_skin']],
        [U, j('skin', 'beard.npz'), j('skin', 'skin.png'), j('skin', 'skin_brows.png')], 'bake, beard offsets, brows'))

    # 7. hair and facial hair as strands
    out.append(Step('hair', [
        ['mkdir', '-p', j('hair', 'cap')],
        # hair is where the full model stands over the head's skin: no colour thresholds
        [PY, os.path.join(HEAD, 'hair', 'hair_layer.py'), V, params, raw['full'], U, GR, j('hair', 'cap'), *place,
         '--ears', j('reg', 'ears.npz')],
        [PY, os.path.join(HEAD, 'hair', 'groom.py'), j('hair', 'cap', 'cap.npz'), U, GR, j('hair'), '90000', '1',
         '--skin', j('skin', 'skin_brows.png'), '--ears', j('reg', 'ears.npz')]],
        [V, params, raw['full'], j('full', 'place.json'), j('full', 'tone.json'), U, GR, j('skin', 'skin_brows.png'), j('reg', 'ears.npz')],
        [j('hair', 'groom.npz')], 'the hairstyle'))
    out.append(Step('beard', [
        [PY, os.path.join(HEAD, 'hair', 'beard.py'), U, GR, V, j('skin', 'skin_brows.png'), j('skin', 'beard.npz'),
         T['points'], j('beard')]],
        [U, GR, V, j('skin', 'skin_brows.png'), j('skin', 'beard.npz'), T['points']],
        [j('beard', 'beard_groom.npz'), j('beard', 'skin_shaved.png')], 'beard, moustache, stubble'))
    out.append(Step('colour', [
        [PY, os.path.join(HEAD, 'concept_colours.py'), j('views'), params, V, j('hair', 'groom.npz'), j('beard', 'beard_groom.npz'),
         T['eye'], j('colour')]],
        [params, V, j('hair', 'groom.npz'), j('beard', 'beard_groom.npz'), T['eye'], EYES]
        + [j('views', n+'.png') for n in ('full_front', 'full_profile', 'bald_front')]
        + [j('views', 'points', n+'.json') for n in ('full_front', 'bald_front')],
        [j('colour', n) for n in ('groom.npz', 'beard_groom.npz', 'hair_ramp.png', 'hair_pigment.png', 'eye.png')],
        'the colours of the hair, the beard and the iris from the concept'))
    eye_map = j('colour', 'eye.png')

    # 8. into the game: the head (twice: the skin material needs the neck cut the export finds), strands
    brows_none = j('views', 'brows_none.png')

    def no_brows():
        from PIL import Image
        Image.new('RGBA', (64, 64), (0, 0, 0, 0)).save(brows_none)
    export = lambda skin, *more: blend(T['probe'], os.path.join(HEAD, 'mpfb', 'export_test_head.py'), content, '--hero', C1, R,
                                       eye_map, '--skin', skin, '--brows', brows_none, *more)
    base = base_cache()
    export_inputs = [T['probe'], eye_map, TEXPACK, os.path.join(HEAD, 'mpfb', 'test-head.face.json'),
                     os.path.join(base, 'models/HUM_HEAD_PONY.MMB.json'), os.path.join(base, 'models/HUM_HEAD_PONY.MMB.bin')]
    out.append(Step('cut', [no_brows, export(j('beard', 'skin_shaved.png'))],
                    [j('round', 'reg.npz'), j('round', 'blink.npz'), j('reg', 'mpfb.npz'), j('beard', 'skin_shaved.png')]
                    + export_inputs, [os.path.join(head, 'head.frame.json')], 'the neck cut', input_root=BLENDER_RESOURCES))
    body = os.path.join(base, 'textures', 'HUM_BODY_NAKED_V8_C0.dds')
    out.append(Step('material', [
        [PY, os.path.join(HEAD, 'skin_material.py'), U, GR, V, j('beard', 'skin_shaved.png'), T['points'], body,
         os.path.join(head, 'head.frame.json'), j('material'), '--groom', j('hair', 'groom.npz'), '--relief', j('relief', 'samples.npz')]],
        [U, GR, V, j('beard', 'skin_shaved.png'), T['points'], body, os.path.join(head, 'head.frame.json'), j('hair', 'groom.npz'),
         j('relief', 'samples.npz')],
        [j('material', n) for n in ('skin_game.png', 'skin_normal.png', 'skin_orm.png', 'skin_height.png', 'skin_sheet.png')],
        'skin for the game'))
    shapes = cfg.get('shapes')
    if shapes not in (None, 'mark'):
        raise SystemExit(f'build_head: "shapes" may only be "mark", not {shapes!r}')
    # with "shapes": the head as exported (MPFB's shapes) is kept in export/, the step "shapes" makes the game's head
    keep = [['cp', os.path.join(head, 'head.glb'), j('export', 'head-mpfb.glb')],
            ['cp', os.path.join(head, 'head.face.json'), j('export', 'head-mpfb.face.json')]] if shapes else []
    out.append(Step('export', [export(j('material', 'skin_game.png'), '--skin-normal', j('material', 'skin_normal.png'),
                                      '--skin-orm', j('material', 'skin_orm.png'))] + keep,
                    [j('round', 'reg.npz'), j('round', 'blink.npz'), j('reg', 'mpfb.npz')] + export_inputs
                    + [j('material', n) for n in ('skin_game.png', 'skin_normal.png', 'skin_orm.png')],
                    [os.path.join(head, 'head.glb')] + [c[2] for c in keep], 'the head', input_root=BLENDER_RESOURCES))
    if shapes == 'mark':
        # 8b. Audio2Face's Mark face shapes instead of MPFB's (reports/content/2026-11-hero-head/face-shapes.md,
        #     "Формы Mark в игре") and the face profile re-scaled for them; NVIDIA's data: the head is not for git
        out.append(Step('shapes', [
            [PY, os.path.join(HEAD, 'shapes', 'head_shapes.py'), j('export', 'head-mpfb.glb'), j('round', 'reg.npz'),
             os.path.join(head, 'head.glb'), '--json', j('shapes', 'info.json'),
             '--face', j('export', 'head-mpfb.face.json'), os.path.join(head, 'head.face.json')]],
            [j('export', 'head-mpfb.glb'), j('export', 'head-mpfb.face.json'), j('round', 'reg.npz'),
             os.path.join(HEAD, 'shapes', 'transfer.py'), os.path.join(HEAD, 'shapes', 'head_glb.py'),
             os.path.join(HEAD, 'shapes', 'mark_profile.py'), os.path.join(HEAD, 'shapes', 'lip_inside.py'),
             os.path.join(HEAD, 'eye_cornea.py'), MARK],
            [j('shapes', 'info.json')], "Mark's face shapes"))

    def install_strands():
        ramp = os.path.join(j('strands'), 'png'); dds = os.path.join(content, 'materials', 'head')
        shutil.rmtree(ramp, ignore_errors=True); os.makedirs(ramp)
        shutil.move(os.path.join(head, 'hair_ramp.png'), os.path.join(ramp, 'hair_ramp.png'))
        subprocess.run([TEXPACK, ramp, dds], check=True, stdout=subprocess.DEVNULL)
        mj = os.path.join(content, 'materials', 'materials.json'); table = json.load(open(mj))
        table['OWN_HERO_HEAD_STRANDS.TGA'] = {'albedo': 'head/hair_ramp.dds', 'hair': True, 'strand': True}
        json.dump(table, open(mj, 'w'), indent=2)
        cj = os.path.join(content, 'characters', 'characters.json'); chars = json.load(open(cj))
        chars[cfg.get('instance', 'PC_HERO')]['groom'] = f'{folder}/head.groom'
        json.dump(chars, open(cj, 'w'), indent=2)
    out.append(Step('strands', [
        [PY, os.path.join(HEAD, 'hair', 'groom_export.py'), os.path.join(head, 'head.frame.json'), U, GR, head,
         '--hair', j('colour', 'groom.npz'), '--beard', j('colour', 'beard_groom.npz'), '--ramp', j('colour', 'hair_ramp.png')],
         install_strands],
        [os.path.join(head, 'head.glb'), os.path.join(head, 'head.frame.json'), U, GR, j('colour', 'groom.npz'),
         j('colour', 'beard_groom.npz'), j('colour', 'hair_ramp.png'), TEXPACK], [os.path.join(head, 'head.groom')], 'strands for the game'))

    # 9. the check: the head with its strands at the concept's cameras next to the concept and the model
    out.append(Step('sheet', [
        # the game's colour map of the skin; under the cut, where the game has its own body, Blender's neck has the
        # colour of that body (skin_material.py's skin_sheet.png). With the map before the game material the sheet
        # showed a pinker head than the game draws (reports/content/2026-11-npc-pipeline/likeness.md)
        blend(j('round', 'reg_sub.blend'), os.path.join(HEAD, 'hair', 'groom_render.py'), V, j('material', 'skin_sheet.png'),
              j('colour', 'groom.npz'), j('sheet'), 's', '--groom', j('colour', 'beard_groom.npz'), '--eye', eye_map,
              '--ramp', j('colour', 'hair_pigment.png'), '--views', 'front,quarter,profile', '--size', '900', '--spp', '128',
              '--height', j('material', 'skin_height.png'), '--light', 'portrait'),
        [PY, os.path.join(SK, 'likeness.py'), V, params, raw['full'], j('views', 'full_front.png'),
         j('views', 'full_profile.png'), j('sheet'), j('sheet', 'likeness.jpg'), *place]],
        [j('round', 'reg_sub.blend'), V, j('material', 'skin_sheet.png'), j('colour', 'groom.npz'), j('colour', 'beard_groom.npz'),
         j('colour', 'hair_pigment.png'), eye_map, params, raw['full'], j('full', 'place.json'), j('full', 'tone.json'),
         j('material', 'skin_height.png')],
        [j('sheet', 'likeness.jpg')], 'Cycles renders and the likeness sheet'))
    lk = j('sheet', 'likeness.jpg')                                   # tiles of 600 px: concept, model, ours
    out.append(Step('measures', [[MPY, os.path.join(HEAD, 'face_points.py'), j('sheet', 'points'),
                                  f'concept={lk}:0,0,600,600', f'ours={lk}:1200,0,1800,600',
                                  'ours_front=' + j('sheet', 's_front.png')],
                                 [PY, os.path.join(HEAD, 'head_measures.py'), work, '--params', params]],
                    [j('reg', 'reg_log.json'), j('round', 'reg.npz'), j('outline', 'outline.json'), j('relief', 'relief.json'),
                     j('reg', 'place.json'), j('full', 'place.json'), j('sheet', 'likeness.jpg'), j('sheet', 's_front.png'),
                     j('reg', 'lid_chains.json'), j('views', 'full_front.png'), j('views', 'points', 'full_front.json'),
                     EYES, params, FACE_LANDMARKER],
                    [j('measures.json')], 'measures of defects'))
    out.append(Step('shape', [[PY, os.path.join(HEAD, 'shape_report.py'), work, '--params', params]],
                    [j('round', 'reg.npz'), j('reg', 'place.json'), j('full', 'place.json'), j('colour', 'groom.npz'),
                     j('colour', 'beard_groom.npz'), params] + [j('views', f'{k}_{v}.png') for k in ('full', 'bald') for v in ('front', 'profile')],
                    [j('shape', 'shape.json')], 'where the shape goes: outlines, hair and beard against the concept'))
    return out


def main(mesh_runner=None):
    a = sys.argv[1:]
    if '--help' in a or '-h' in a:
        print(__doc__)
        return

    def opt(name, default=None):
        if name not in a: return default
        i = a.index(name); v = a[i + 1]; del a[i:i + 2]; return v
    lst = '--list' in a
    if lst: a.remove('--list')
    work, content, frm, only = opt('--work'), opt('--content'), opt('--from'), opt('--only')
    gpu = opt('--gpu-authorization')
    mesh_runner = opt('--mesh-runner', mesh_runner)
    if len(a) != 1: raise SystemExit(__doc__)
    cdir = os.path.abspath(a[0]); cfg = json.load(open(os.path.join(cdir, 'npc.json')))
    if cfg.get('instance', 'PC_HERO') != 'PC_HERO': raise SystemExit('build_head: only PC_HERO can be exported for now')
    work = os.path.abspath(os.path.expanduser(work or os.path.join(MPFB, 'work', 'build', cfg['name'])))
    content = os.path.abspath(os.path.expanduser(content or os.path.join(work, 'content')))
    for d in ('views/full', 'views/bald', 'mesh'): os.makedirs(os.path.join(work, d), exist_ok=True)
    os.makedirs(os.path.join(content, 'characters'), exist_ok=True)
    plan = steps(cdir, cfg, work, content, gpu, mesh_runner); names = [s.name for s in plan]
    if lst: print(' '.join(names)); return
    for n in (frm, only):
        if n and n not in names: raise SystemExit(f'build_head: no step {n}; steps: {" ".join(names)}')
    t0 = time.time()
    for i, s in enumerate(plan):
        if only and s.name != only: continue
        if frm and i < names.index(frm): continue
        if frm and s.name == frm or only:
            sp = os.path.join(work, s.name, '.stamp')
            if os.path.exists(sp): os.remove(sp)
        run(s, work)
    print(f'build_head: done in {time.time()-t0:.0f} s\n  content {content}\n  sheet {work}/sheet/likeness.jpg\n'
          f'  measures {work}/measures.json\n  shape {work}/shape/shape.jpg')


if __name__ == '__main__':
    main()
