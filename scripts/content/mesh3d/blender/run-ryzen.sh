#!/bin/sh
# Arguments refer to absolute paths on Ryzen, including all inputs and outputs.
# Example: run-ryzen.sh postprocess --raw /home/user/gothic-ai/mesh3d/raw.npz ...
set -eu

module_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(CDPATH= cd -- "$module_dir/../../../.." && pwd)
mode=${1:-}
case "$mode" in
    postprocess|parametric|sheet|synthetic) shift ;;
    *) echo "Usage: $0 {postprocess|parametric|sheet|synthetic} [script arguments]" >&2; exit 2 ;;
esac

path_flag=
for argument in "$@"; do
    if [ -n "$path_flag" ]; then
        case "$argument" in
            /*) ;;
            *) echo "$path_flag requires an absolute path on Ryzen: $argument" >&2; exit 2 ;;
        esac
        path_flag=
        continue
    fi
    case "$argument" in
        --raw|--meta|--target|--out|--variants) path_flag=$argument ;;
        --raw=*|--meta=*|--target=*|--out=*|--variants=*)
            case "${argument#*=}" in
                /*) ;;
                *) echo "Requires an absolute path on Ryzen: $argument" >&2; exit 2 ;;
            esac ;;
    esac
done
if [ -n "$path_flag" ]; then
    echo "$path_flag requires an absolute path on Ryzen." >&2
    exit 2
fi

remote_root=$(ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen \
    'printf "%s" "$HOME/gothic-ai/mesh3d"')
case "$remote_root" in
    /*) ;;
    *) echo 'Could not resolve an absolute mesh3d directory on Ryzen.' >&2; exit 1 ;;
esac
# Resolve shell quoting once, before the SSH command interprets the path.
remote_scripts=$(printf '%s' "$remote_root/scripts" | sed "s/'/'\\\\''/g")
ssh -T -o BatchMode=yes -o ConnectTimeout=10 ryzen "mkdir -p '$remote_scripts'"
scp -q "$module_dir/"*.py "ryzen:$remote_root/scripts/"

exec "$repo_root/scripts/gq" run --class batch --threads 6 --mem 8G \
    --gpu-mem 0 --max 30m --name "mesh3d-$mode" -- \
    env LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=llvmpipe \
    MESA_LOADER_DRIVER_OVERRIDE=llvmpipe __GLX_VENDOR_LIBRARY_NAME=mesa \
    CUDA_VISIBLE_DEVICES= HIP_VISIBLE_DEVICES= OMP_NUM_THREADS=6 \
    "$remote_root/blender/blender" -b --factory-startup --threads 6 \
    --python-exit-code 1 -P "$remote_root/scripts/$mode.py" -- "$@"
