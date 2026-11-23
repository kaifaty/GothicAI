"""External inputs shared by the head builder, its Python tools and Blender."""
import os
import shutil


def env_path(name, default):
    return os.path.abspath(os.path.expanduser(os.environ.get(name) or default))


def executable(name, default):
    program = os.path.expanduser(os.environ.get(name) or default)
    return os.path.abspath(shutil.which(program) or program)


DATA_ROOT = env_path('GOTHIC_AI_DATA', '~/.local/share/gothic-ai')
MPFB = os.path.join(DATA_ROOT, 'mpfb')
BLENDER_RESOURCES = env_path('BLENDER_USER_RESOURCES', os.path.join(MPFB, 'blender'))
MPFB_DATA = os.path.join(BLENDER_RESOURCES, 'extensions/.user/user_default/mpfb/data')
TEXPACK = executable('TEXPACK', os.path.join(DATA_ROOT, 'hd-textures/bin/texpack'))
FACE_LANDMARKER = env_path('FACE_LANDMARKER', os.path.join(DATA_ROOT, 'models/mediapipe/face_landmarker.task'))
FACE_SOURCES = os.path.join(DATA_ROOT, 'face-sources')
MARK = os.path.join(FACE_SOURCES, 'mark/bs_skin_Mark.npz')


def base_cache_root():
    return env_path('OPENGOTHIC_BASE_CACHE', '~/.cache/gothic-ai/base')


def base_cache():
    root = base_cache_root()
    dirs = [os.path.join(root, d) for d in os.listdir(root) if os.path.isdir(os.path.join(root, d, 'models'))]
    if not dirs:
        raise SystemExit('build_head: no imported game base in ' + root)
    return max(dirs, key=os.path.getmtime)
