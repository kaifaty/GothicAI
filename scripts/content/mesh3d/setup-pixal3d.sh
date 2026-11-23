#!/usr/bin/env bash
# Pixal3D (TencentARC, MIT code and weights) next to TRELLIS.2 on dev-ubuntu: its own venv, pinned sources and
# weights; installation and downloads only, never stops services or runs generation (run-mesh3d.sh does).
# From the Mac: bash scripts/content/mesh3d/setup-pixal3d.sh
#
# Pixal3D is TRELLIS.2 with pixel-aligned image features, plus a multi-view mode with known cameras (the
# city-furniture pilot, 2026-11-02). Its upstream setup pulls RMBG-2.0 (CC BY-NC) and nvdiffrast (NVIDIA
# non-commercial): neither is installed or downloaded here, as for TRELLIS.2 (setup-dev-ubuntu.sh). The venv is on
# torch 2.7 because NATTEN, which the NAF feature upsampler needs, ships wheels for it; TRELLIS.2's venv is untouched.
set -euo pipefail
[[ $# -eq 0 ]] || exit 2
script_dir="$(cd "$(dirname "$0")" && pwd)"
if [[ "$(uname -s)" == Darwin ]]; then
  ssh dev-ubuntu mkdir -p gothic-ai/mesh3d
  scp "$script_dir/adapter.py" "$script_dir/generate_trellis.py" "$script_dir/generate_pixal3d.py" \
    "$script_dir/download_weights.py" "$script_dir/setup-pixal3d.sh" dev-ubuntu:gothic-ai/mesh3d/
  exec ssh -T dev-ubuntu bash gothic-ai/mesh3d/setup-pixal3d.sh
fi
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || exit 2
root="$HOME/gothic-ai/mesh3d"
cd "$root"
[[ -x toolchain/bin/nvcc && -d repos/TRELLIS.2/o-voxel ]] || { echo 'Run setup-dev-ubuntu.sh first' >&2; exit 1; }
export HF_HUB_DISABLE_IMPLICIT_TOKEN=1 HF_HOME="$root/hf"
export TORCH_HOME="$root/cache/torch" XDG_CACHE_HOME="$root/cache" TMPDIR="$root/tmp"
export TORCH_CUDA_ARCH_LIST=8.6 MAX_JOBS=2
uv="$HOME/.local/bin/uv"
df -h "$root"

checkout() {
  local name="$1" url="$2" revision="$3"
  if [[ ! -d "repos/$name/.git" ]]; then
    git init -q "repos/$name"
    git -C "repos/$name" remote add origin "$url"
  fi
  git -C "repos/$name" fetch --depth 1 origin "$revision"
  git -C "repos/$name" checkout --detach "$revision"
}
checkout Pixal3D https://github.com/TencentARC/Pixal3D.git f7cf38429b0bd264f1995f0f8743a88b1c728b94
checkout NAF https://github.com/valeoai/NAF.git 37f2dfc180f2de53d98bd601109c0da0dd6b0f43

export CUDA_HOME="$root/toolchain"
export CC="$CUDA_HOME/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$CUDA_HOME/bin/x86_64-conda-linux-gnu-g++" CUDACXX="$CUDA_HOME/bin/nvcc"
export PATH="$CUDA_HOME/bin:$PATH"
export CPATH="$CUDA_HOME/targets/x86_64-linux/include"
export LIBRARY_PATH="$CUDA_HOME/lib:$CUDA_HOME/targets/x86_64-linux/lib"
export LD_LIBRARY_PATH="$LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

python="$root/.venv-pixal3d/bin/python"
[[ -x "$python" ]] || "$uv" venv --python 3.10.21 "$root/.venv-pixal3d"
"$uv" pip install --python "$python" torch==2.7.0 torchvision==0.22.0 xformers==0.0.30 \
  --index-url https://download.pytorch.org/whl/cu126
"$uv" pip install --python "$python" \
  numpy==1.26.4 scipy==1.15.3 pillow==12.0.0 imageio==2.37.2 imageio-ffmpeg==0.6.0 \
  tqdm==4.67.1 easydict==1.13 opencv-python-headless==4.11.0.86 \
  ninja==1.13.0 setuptools==80.9.0 wheel==0.45.1 packaging==25.0 \
  trimesh==4.10.1 transformers==4.57.3 huggingface-hub==0.36.0 safetensors==0.7.0 \
  timm==1.0.22 kornia==0.8.2 zstandard==0.25.0 einops==0.8.1
"$uv" pip install --python "$python" \
  "https://github.com/SHI-Labs/NATTEN/releases/download/v0.21.0/natten-0.21.0%2Btorch270cu126-cp310-cp310-linux_x86_64.whl"
"$uv" pip install --python "$python" --no-build-isolation "$root/repos/utils3d"
for include in "$root/.venv-pixal3d/lib/python3.10/site-packages/nvidia/"*/include; do
  export CPATH="$CPATH:$include"
done
for extension in FlexGEMM CuMesh TRELLIS.2/o-voxel; do
  "$uv" pip install --python "$python" --no-build-isolation --no-deps --reinstall "$root/repos/$extension"
done
"$uv" run --python 3.10.21 --with huggingface-hub==0.36.0 python "$root/download_weights.py" pixal3d --root "$root"
HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 "$python" "$root/generate_pixal3d.py" --check --root "$root"
"$uv" pip list --python "$python"
df -h "$root"
