#!/usr/bin/env bash
# Installation/downloads and imports; never stops services or runs generation.
# From the Mac: bash scripts/content/mesh3d/setup-dev-ubuntu.sh [--skip-dinov3]
# On dev-ubuntu: bash ~/gothic-ai/mesh3d/setup-dev-ubuntu.sh [--skip-dinov3|--dinov3-only]
set -euo pipefail
mode="${1:-all}"
case "$mode" in all|--skip-dinov3|--dinov3-only) ;; *) exit 2 ;; esac
[[ $# -le 1 ]] || exit 2
script_dir="$(cd "$(dirname "$0")" && pwd)"
if [[ "$(uname -s)" == Darwin ]]; then
  ssh dev-ubuntu mkdir -p gothic-ai/mesh3d
  scp "$script_dir/adapter.py" "$script_dir/generate_trellis.py" \
    "$script_dir/check_imports.py" "$script_dir/download_weights.py" \
    "$script_dir/setup-dev-ubuntu.sh" dev-ubuntu:gothic-ai/mesh3d/
  exec ssh -T dev-ubuntu bash gothic-ai/mesh3d/setup-dev-ubuntu.sh "$mode"
fi
[[ "$(uname -s)" == Linux && "$(uname -m)" == x86_64 ]] || exit 2
root="$HOME/gothic-ai/mesh3d"
mkdir -p "$root" "$root/tmp" "$root/repos" "$root/cache/torch"
cd "$root"
export HF_HUB_DISABLE_IMPLICIT_TOKEN=1 HF_HOME="$root/hf"
export TORCH_HOME="$root/cache/torch" XDG_CACHE_HOME="$root/cache" TMPDIR="$root/tmp"
export MAMBA_ROOT_PREFIX="$root/mamba"
export TORCH_CUDA_ARCH_LIST=8.6 ATTN_BACKEND=xformers MAX_JOBS=2
export FLEX_GEMM_USE_AUTOTUNE_CACHE=0 FLEX_GEMM_AUTOSAVE_AUTOTUNE_CACHE=0
uv="$HOME/.local/bin/uv"
[[ -x "$uv" ]] || { echo 'Install uv in ~/.local/bin first' >&2; exit 1; }
if [[ "$mode" == --dinov3-only ]]; then
  exec "$uv" run --python 3.10.21 --with huggingface-hub==0.36.0 python "$root/download_weights.py" dinov3 --root "$root"
fi
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
checkout TRELLIS.2 https://github.com/microsoft/TRELLIS.2.git 75fbf0183001ed9876c8dbb35de6b68552ee08bd
checkout TRELLIS https://github.com/microsoft/TRELLIS.git 442aa1e1afb9014e80681d3bf604e8d728a86ee7
checkout CuMesh https://github.com/JeffreyXiang/CuMesh.git 12289e1062f0603f2f0d0771b02e1395d247f26f
checkout FlexGEMM https://github.com/JeffreyXiang/FlexGEMM.git 6dd94a859c26ee8246888502eada3dd8ad85532e
checkout dinov2 https://github.com/facebookresearch/dinov2.git 85a24602099d397264d5b30461ad7f3bfd726ca1
checkout utils3d https://github.com/EasternJournalist/utils3d.git 9a4eb15e4021b67b12c460c7057d642626897ec8
git -C repos/TRELLIS submodule update --init --depth 1 trellis/representations/mesh/flexicubes
git -C repos/TRELLIS.2 submodule update --init --depth 1
git -C repos/CuMesh submodule update --init --recursive --depth 1

# Compiler/CUDA live entirely under this task, independently of system CUDA 13/gcc 15.
if [[ ! -x bin/micromamba ]]; then
  mkdir -p bin
  curl -fLsS https://micro.mamba.pm/api/micromamba/linux-64/2.8.1 | tar -xj bin/micromamba
fi
if [[ ! -x toolchain/bin/nvcc || ! -x toolchain/bin/x86_64-conda-linux-gnu-g++ ]]; then
  ./bin/micromamba create -y -p "$root/toolchain" -c conda-forge \
    cuda-nvcc=12.4.131 cuda-cudart-dev=12.4.127 cuda-cccl=12.4.127 gxx_linux-64=12.4.0
fi
export CUDA_HOME="$root/toolchain"
export CC="$CUDA_HOME/bin/x86_64-conda-linux-gnu-gcc"
export CXX="$CUDA_HOME/bin/x86_64-conda-linux-gnu-g++" CUDACXX="$CUDA_HOME/bin/nvcc"
export PATH="$CUDA_HOME/bin:$PATH"
export CPATH="$CUDA_HOME/targets/x86_64-linux/include"
export LIBRARY_PATH="$CUDA_HOME/lib:$CUDA_HOME/targets/x86_64-linux/lib"
export LD_LIBRARY_PATH="$LIBRARY_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
"$CUDACXX" --version
"$CXX" --version

for model in trellis2 trellis; do
  python="$root/.venv-$model/bin/python"
  [[ -x "$python" ]] || "$uv" venv --python 3.10.21 "$root/.venv-$model"
  "$uv" pip install --python "$python" torch==2.6.0 torchvision==0.21.0 xformers==0.0.29.post3 \
    --index-url https://download.pytorch.org/whl/cu124
  "$uv" pip install --python "$python" \
    numpy==1.26.4 scipy==1.15.3 pillow==11.3.0 imageio==2.37.0 imageio-ffmpeg==0.6.0 \
    tqdm==4.67.1 easydict==1.13 opencv-python-headless==4.11.0.86 \
    ninja==1.13.0 setuptools==80.9.0 wheel==0.45.1 packaging==25.0 \
    trimesh==4.9.0 transformers==4.57.1 huggingface-hub==0.36.0 safetensors==0.6.2 \
    timm==1.0.22 kornia==0.8.2 zstandard==0.25.0 spconv-cu120==2.3.6
  "$uv" pip install --python "$python" --no-build-isolation "$root/repos/utils3d"
done
python="$root/.venv-trellis2/bin/python"
# CUDA runtime wheels also carry cuBLAS/cuSPARSE headers used by CuMesh's BVH.
for include in "$root/.venv-trellis2/lib/python3.10/site-packages/nvidia/"*/include; do
  export CPATH="$CPATH:$include"
done
# Do not invoke upstream setup.sh: it installs noncommercial rendering/rembg packages.
for extension in FlexGEMM CuMesh TRELLIS.2/o-voxel; do
  "$uv" pip install --python "$python" --no-build-isolation --no-deps "$root/repos/$extension"
done
"$python" "$root/download_weights.py" public --root "$root"
for model in trellis2 trellis; do
  HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 ".venv-$model/bin/python" \
    "$root/check_imports.py" --root "$root" --model "$model"
  "$uv" pip list --python ".venv-$model/bin/python"
done
if [[ "$mode" != --skip-dinov3 ]]; then
  "$python" "$root/download_weights.py" dinov3 --root "$root"
fi
