# Installs the MPFB asset packs (system assets, visemes02, faceunits01) from ~/.local/share/gothic-ai/mpfb/downloads
# into MPFB's user data of the separate Blender profile, after MPFB's own zip check.
#   BLENDER_USER_RESOURCES=~/.local/share/gothic-ai/mpfb/blender blender -b -P install_packs.py
import sys, zipfile, os
from bl_ext.user_default.mpfb.services import LocationService, AssetService
data=LocationService.get_user_data(); print('USERDATA', data)
D=os.path.expanduser('~/.local/share/gothic-ai/mpfb/downloads')
for z in ('makehuman_system_assets_cc0.zip','visemes02.zip','faceunits01.zip'):
    p=os.path.join(D,z); chk=AssetService.check_asset_pack_zip(p); print('CHECK',z,chk)
    if chk is None:
        with zipfile.ZipFile(p) as f: f.extractall(data)
AssetService.update_all_asset_lists(); print('DONE')
