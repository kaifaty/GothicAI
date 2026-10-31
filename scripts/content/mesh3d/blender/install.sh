#!/bin/sh
# Run on Ryzen: ssh ryzen bash < scripts/content/mesh3d/blender/install.sh
set -eu

version=5.2.2
archive="blender-${version}-linux-x64.tar.xz"
sha256=84098912789dc450e95697c4184fb8a90acbe5111c2ba4aede3fecb57806a168
release_url=https://download.blender.org/release/Blender5.2
install_root="$HOME/gothic-ai/mesh3d/blender"
version_dir="$install_root/blender-${version}-linux-x64"

case "$(uname -sm)" in
    'Linux x86_64') ;;
    *) echo 'install.sh requires Linux x86_64 (Ryzen).' >&2; exit 2 ;;
esac

# gq itself needs 20 GiB free. Reserve another 3 GiB for download and extraction.
df -h "$HOME"
if [ -x "$version_dir/blender" ]; then
    ln -sfn "blender-${version}-linux-x64/blender" "$install_root/blender"
    "$version_dir/blender" --version
    printf '%s\n' "$version_dir/blender"
    exit 0
fi
available_kib=$(df -Pk "$HOME" | awk 'NR == 2 {print $4}')
if [ "$available_kib" -lt 24117248 ]; then
    echo "Need 23 GiB free before installation; available: $available_kib KiB." >&2
    exit 1
fi

mkdir -p "$install_root"
staging=$(mktemp -d "$install_root/.install.XXXXXX")
trap 'rm -rf -- "$staging"' EXIT HUP INT TERM
curl --fail --location --retry 3 "$release_url/blender-${version}.sha256" \
    --output "$staging/checksums.sha256"
official_sha256=$(awk -v name="$archive" '$2 == name {print $1}' "$staging/checksums.sha256")
if [ "$official_sha256" != "$sha256" ]; then
    echo 'Official Blender checksum differs from the pinned release checksum.' >&2
    exit 1
fi
curl --fail --location --retry 3 "$release_url/$archive" --output "$staging/$archive"
(cd "$staging" && printf '%s  %s\n' "$sha256" "$archive" | sha256sum --check -)
tar -xJf "$staging/$archive" -C "$staging"
mv "$staging/blender-${version}-linux-x64" "$version_dir"
ln -sfn "blender-${version}-linux-x64/blender" "$install_root/blender"
"$version_dir/blender" --version
df -h "$HOME"
printf '%s\n' "$install_root/blender"
